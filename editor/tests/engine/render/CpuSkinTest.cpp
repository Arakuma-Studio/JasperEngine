// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CpuSkin (vertex animation spec 3.2.2): the full-mesh CPU evaluator of the vertex stage. Every
// deformed vertex is compared with a reference computed here with plain double math from the
// glTF data (or the arrays a generated glTF was written from): joints' world matrices from the
// Transform TRS chains, times the inverse binds, blended by the weights, after the morph targets.
// Covered: the source as the GPU reads it (the hero's three submeshes sharing one vertex buffer,
// an interleaved glTF with normalized integer attributes, byte and 32-bit indices, the procedural
// box's own buffers), the bind pose, clip times, the legacy bone path, pose::PoseSkin's
// subsample, morph targets (2, 4 and 8 target layouts), tangents keeping w, stage-0 and stage-1
// lattices against lattice::deformVertex (a rigid mesh's, a colour mask, one submesh only) and
// every unsupported layout's error. A headless project with the pose-animation, shader-library
// and lattice fixtures.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Out.h"
#include "Project.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipConverters.h"
#include "buffer/ExternalBuffer.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "component/BoneComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "lattice/LatticeRender.h"
#include "lattice/LatticeSolver.h"
#include "pose/ModelRig.h"
#include "pose/PoseSkin.h"
#include "render/CpuSkin.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"

#include "json.hpp"
#include "tiny_gltf.h"

#include "support/PosixCompat.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::cpuskin;
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
            const fs::path root = fs::temp_directory_path() / ("jasper-cpuskin-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets" / "models" / "gen");
            std::ofstream(out.dir / "project.yaml") << "name: cpuskin\nassetsDir: assets\n";
            const fs::path models = out.dir / "assets" / "models";
            const fs::path pose = fs::path("tools") / "fixtures" / "pose-animation" / "assets" / "models";
            // T1c's animated hero (node hierarchy: Walk, Idle, Wave ...), the T-pose hero (legacy
            // layout), a skinned strip with one morph target, the lattice tests' skinned column,
            // a rigid figure of three submeshes sharing its vertex buffers
            fs::copy_file(repoFile(pose / "hero_animated.gltf"), models / "hero.gltf", fs::copy_options::overwrite_existing);
            fs::copy_file(repoFile(pose / "hero_tpose.gltf"), models / "tpose.gltf", fs::copy_options::overwrite_existing);
            fs::copy_file(repoFile("tools/fixtures/shader-library/assets/models/skinned_morph.gltf"), models / "strip.gltf",
                          fs::copy_options::overwrite_existing);
            fs::copy_file(repoFile("engine/tests/htmlui/fixtures/lattice/skinned_column.gltf"), models / "column.gltf",
                          fs::copy_options::overwrite_existing);
            fs::copy_file(repoFile("tools/fixtures/extra-pass/assets/models/figure.gltf"), models / "figure.gltf",
                          fs::copy_options::overwrite_existing);
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
    CommandHistory* history(uint32_t sceneId) { return CommandHandle::get(sceneId); }
    Scene* sceneOf(uint32_t sceneId) { return project()->getScene(sceneId)->scene; }

    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("CpuSkin", SceneType::SCENE_3D);
        return p->getSelectedSceneId();
    }

    Entity loadModel(uint32_t sceneId, const std::string& assetsRelative) {
        CreateEntityCmd* create = new CreateEntityCmd(project(), sceneId, "Model", EntityCreationType::MODEL);
        create->setQuiet(true);
        history(sceneId)->addCommandNoMerge(create);
        const Entity entity = create->getEntity();
        const uint64_t logs = Out::lastLogId();
        history(sceneId)->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, entity, assetsRelative));
        const ModelComponent& model = sceneOf(sceneId)->getComponent<ModelComponent>(entity);
        std::string log;
        for (const editor::LogEntry& entry : Out::getLogsSince(logs)) log += entry.message + "\n";
        EXPECT_FALSE(model.loadedFilename.empty()) << "loaded " << assetsRelative << ": " << log;
        return entity;
    }

    // the entity holding the model's geometry: the model itself, or its (first) mesh node
    Entity meshEntityOf(Scene* scene, Entity model) {
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(model);
        if (mesh && mesh->numSubmeshes > 0) return model;
        for (const auto& node : scene->getComponent<ModelComponent>(model).meshNodesMapping) {
            const MeshComponent* child = scene->findComponent<MeshComponent>(node.second);
            if (child && child->numSubmeshes > 0) return node.second;
        }
        return NULL_ENTITY;
    }

    Entity jointEntity(const ModelComponent& model, int node) {
        auto found = model.nodesIdMapping.find(node);
        if (found != model.nodesIdMapping.end()) return found->second;
        auto bone = model.bonesIdMapping.find(node);
        return bone != model.bonesIdMapping.end() ? bone->second : NULL_ENTITY;
    }

    Quaternion axisAngle(double degrees, double x, double y, double z) {
        const double half = degrees * M_PI / 360.0;
        const double length = std::sqrt(x * x + y * y + z * z);
        const double s = std::sin(half) / length;
        return Quaternion((float)std::cos(half), (float)(x * s), (float)(y * s), (float)(z * s));
    }

    // ---------------------------------------------------------------- double reference math

    struct M4 {
        double m[4][4]; // [column][row]
    };

    M4 identity() {
        M4 out{};
        for (int i = 0; i < 4; i++) out.m[i][i] = 1.0;
        return out;
    }

    M4 mul(const M4& a, const M4& b) {
        M4 out{};
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) out.m[c][r] += a.m[k][r] * b.m[c][k];
        return out;
    }

    M4 fromMatrix(const Matrix4& matrix) {
        M4 out{};
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) out.m[c][r] = matrix.get(c, r);
        return out;
    }

    M4 fromColumns(const float* values) {
        M4 out{};
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) out.m[c][r] = values[c * 4 + r];
        return out;
    }

    M4 inverse(const M4& in) {
        double a[4][8];
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                a[r][c] = in.m[c][r];
                a[r][c + 4] = (r == c) ? 1.0 : 0.0;
            }
        }
        for (int col = 0; col < 4; col++) {
            int pivot = col;
            for (int r = col + 1; r < 4; r++)
                if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
            for (int c = 0; c < 8; c++) std::swap(a[col][c], a[pivot][c]);
            const double d = a[col][col];
            for (int c = 0; c < 8; c++) a[col][c] /= d;
            for (int r = 0; r < 4; r++) {
                if (r == col) continue;
                const double f = a[r][col];
                for (int c = 0; c < 8; c++) a[r][c] -= f * a[col][c];
            }
        }
        M4 out{};
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) out.m[c][r] = a[r][c + 4];
        return out;
    }

    M4 trs(const Vector3& t, const Quaternion& q, const Vector3& s) {
        const double w = q.w, x = q.x, y = q.y, z = q.z;
        const double r[3][3] = {
            {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
        const double scale[3] = {s.x, s.y, s.z};
        M4 out = identity();
        for (int c = 0; c < 3; c++)
            for (int row = 0; row < 3; row++) out.m[c][row] = r[row][c] * scale[c];
        out.m[3][0] = t.x;
        out.m[3][1] = t.y;
        out.m[3][2] = t.z;
        return out;
    }

    // the entity's world matrix from the TRS of every Transform up its chain (not modelMatrix)
    M4 worldOf(Scene* scene, Entity entity) {
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) return identity();
        const M4 local = trs(transform->position, transform->rotation, transform->scale);
        if (transform->parent == NULL_ENTITY) return local;
        return mul(worldOf(scene, transform->parent), local);
    }

    Vector3 point(const M4& m, const Vector3& p) {
        double v[4];
        for (int r = 0; r < 4; r++) v[r] = m.m[0][r] * p.x + m.m[1][r] * p.y + m.m[2][r] * p.z + m.m[3][r];
        return Vector3((float)(v[0] / v[3]), (float)(v[1] / v[3]), (float)(v[2] / v[3]));
    }

    Vector3 direction(const M4& m, const Vector3& d) {
        double v[3];
        for (int r = 0; r < 3; r++) v[r] = m.m[0][r] * d.x + m.m[1][r] * d.y + m.m[2][r] * d.z;
        const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return Vector3((float)(v[0] / length), (float)(v[1] / length), (float)(v[2] / length));
    }

    Vector3 unit(const Vector3& v) {
        const double length = std::sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z);
        return Vector3((float)(v.x / length), (float)(v.y / length), (float)(v.z / length));
    }

    // sum of weights[k] * mats[joints[k]] (weights as decoded, any scale: positions divide by w)
    M4 blend(const std::vector<M4>& mats, const int joints[4], const double weights[4]) {
        M4 out{};
        for (int k = 0; k < 4; k++) {
            if (weights[k] == 0.0) continue;
            const M4& m = mats.at(static_cast<size_t>(joints[k]));
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) out.m[c][r] += m.m[c][r] * weights[k];
        }
        return out;
    }

    double distance(const Vector3& a, const Vector3& b) {
        const double dx = (double)a.x - b.x, dy = (double)a.y - b.y, dz = (double)a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // the largest difference over a set, and where
    struct MaxError {
        double value = 0.0;
        size_t at = 0;
        void add(double e, size_t i) {
            if (!(e <= value)) {
                value = e;
                at = i;
            }
        }
    };

    // ---------------------------------------------------------------- glTF accessors (plain reads)

    std::vector<double> readAccessor(const tinygltf::Model& g, int index, int& components) {
        const tinygltf::Accessor& accessor = g.accessors.at(static_cast<size_t>(index));
        EXPECT_FALSE(accessor.sparse.isSparse) << "accessor " << index;
        const tinygltf::BufferView& view = g.bufferViews.at(static_cast<size_t>(accessor.bufferView));
        const std::vector<unsigned char>& data = g.buffers.at(static_cast<size_t>(view.buffer)).data;
        components = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
        const int size = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
        const int stride = accessor.ByteStride(view);
        std::vector<double> out(accessor.count * static_cast<size_t>(components));
        for (size_t e = 0; e < accessor.count; e++) {
            const unsigned char* p = data.data() + view.byteOffset + accessor.byteOffset + e * static_cast<size_t>(stride);
            for (int c = 0; c < components; c++) {
                const unsigned char* v = p + c * size;
                double value = 0.0;
                switch (accessor.componentType) {
                    case TINYGLTF_COMPONENT_TYPE_FLOAT: { float f; std::memcpy(&f, v, 4); value = f; break; }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: value = accessor.normalized ? *v / 255.0 : *v; break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t u; std::memcpy(&u, v, 2); value = accessor.normalized ? u / 65535.0 : u; break; }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: { uint32_t u; std::memcpy(&u, v, 4); value = u; break; }
                    default: ADD_FAILURE() << "component type " << accessor.componentType; break;
                }
                out[e * static_cast<size_t>(components) + static_cast<size_t>(c)] = value;
            }
        }
        return out;
    }

    std::vector<double> readAccessor(const tinygltf::Model& g, int index) {
        int components = 0;
        return readAccessor(g, index, components);
    }

    // The skinned/morphed mesh of a loaded glTF model by plain math, compared with CpuSkin's
    // result for the mesh entity: per vertex, its submesh's primitive (found by its index list)
    // at its element index
    struct GltfComparison {
        MaxError position;
        MaxError normal;
        double moved = 0.0; // largest distance from the bind position
        size_t vertices = 0;
    };

    GltfComparison compareWithGltf(Scene* scene, Entity modelEntity, Entity meshEntity, const CpuSkinSource& source,
                                   const CpuSkinResult& result) {
        GltfComparison out;
        const ModelComponent& model = scene->getComponent<ModelComponent>(modelEntity);
        const tinygltf::Model& g = *model.gltfModel;
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(meshEntity);

        int node = -1;
        for (const auto& entry : model.meshNodesMapping)
            if (entry.second == meshEntity) node = entry.first;
        if (node < 0) {
            for (size_t n = 0; n < g.nodes.size() && node < 0; n++)
                if (g.nodes[n].mesh >= 0) node = static_cast<int>(n);
        }
        EXPECT_GE(node, 0);
        if (node < 0) return out;
        const tinygltf::Mesh& gmesh = g.meshes.at(static_cast<size_t>(g.nodes[static_cast<size_t>(node)].mesh));
        const int skinIndex = g.nodes[static_cast<size_t>(node)].skin;

        // joint matrices: inverse(mesh world) * joint world * inverse bind
        std::vector<M4> mats;
        if (skinIndex >= 0) {
            const tinygltf::Skin& skin = g.skins.at(static_cast<size_t>(skinIndex));
            const std::vector<double> ibm = readAccessor(g, skin.inverseBindMatrices);
            const M4 inverseMesh = inverse(worldOf(scene, meshEntity));
            for (size_t j = 0; j < skin.joints.size(); j++) {
                float columns[16];
                for (int i = 0; i < 16; i++) columns[i] = (float)ibm[j * 16 + static_cast<size_t>(i)];
                const Entity joint = jointEntity(model, skin.joints[j]);
                EXPECT_NE(joint, NULL_ENTITY) << "joint " << j;
                mats.push_back(mul(mul(inverseMesh, worldOf(scene, joint)), fromColumns(columns)));
            }
        }

        for (const CpuSkinSubmesh& submesh : source.submeshes) {
            // the primitive drawing the same indices
            std::vector<uint32_t> drawn;
            for (uint32_t index : submesh.indices) drawn.push_back(source.vertexIndex[index]);
            int primitive = -1;
            for (size_t p = 0; p < gmesh.primitives.size() && primitive < 0; p++) {
                const std::vector<double> indices = readAccessor(g, gmesh.primitives[p].indices);
                if (indices.size() != drawn.size()) continue;
                bool same = true;
                for (size_t k = 0; k < drawn.size() && same; k++) same = (uint32_t)indices[k] == drawn[k];
                if (same) primitive = static_cast<int>(p);
            }
            EXPECT_GE(primitive, 0) << "submesh " << submesh.submesh << " draws the indices of no primitive";
            if (primitive < 0) continue;
            const tinygltf::Primitive& prim = gmesh.primitives[static_cast<size_t>(primitive)];
            const std::vector<double> positions = readAccessor(g, prim.attributes.at("POSITION"));
            const bool hasNormal = prim.attributes.count("NORMAL") > 0;
            const std::vector<double> normals = hasNormal ? readAccessor(g, prim.attributes.at("NORMAL")) : std::vector<double>();
            const bool skinned = prim.attributes.count("JOINTS_0") && prim.attributes.count("WEIGHTS_0") && !mats.empty();
            const std::vector<double> joints = skinned ? readAccessor(g, prim.attributes.at("JOINTS_0")) : std::vector<double>();
            const std::vector<double> weights = skinned ? readAccessor(g, prim.attributes.at("WEIGHTS_0")) : std::vector<double>();
            std::vector<std::vector<double>> morphPositions, morphNormals;
            for (const auto& target : prim.targets) {
                morphPositions.push_back(target.count("POSITION") ? readAccessor(g, target.at("POSITION")) : std::vector<double>());
                morphNormals.push_back(target.count("NORMAL") ? readAccessor(g, target.at("NORMAL")) : std::vector<double>());
            }

            for (uint32_t v = submesh.firstVertex; v < submesh.firstVertex + submesh.vertexCount; v++) {
                const size_t e = source.vertexIndex[v];
                double p[3] = {positions[e * 3], positions[e * 3 + 1], positions[e * 3 + 2]};
                double n[3] = {0, 0, 0};
                if (hasNormal) for (int c = 0; c < 3; c++) n[c] = normals[e * 3 + static_cast<size_t>(c)];
                for (size_t t = 0; t < morphPositions.size() && t < (size_t)MAX_MORPHTARGETS; t++) {
                    for (int c = 0; c < 3; c++) {
                        if (!morphPositions[t].empty()) p[c] += mesh.morphWeights[t] * morphPositions[t][e * 3 + static_cast<size_t>(c)];
                        if (!morphNormals[t].empty()) n[c] += mesh.morphWeights[t] * morphNormals[t][e * 3 + static_cast<size_t>(c)];
                    }
                }
                Vector3 position((float)p[0], (float)p[1], (float)p[2]);
                Vector3 normal((float)n[0], (float)n[1], (float)n[2]);
                if (skinned) {
                    int ids[4];
                    double w[4];
                    for (int k = 0; k < 4; k++) {
                        ids[k] = (int)joints[e * 4 + static_cast<size_t>(k)];
                        w[k] = weights[e * 4 + static_cast<size_t>(k)];
                    }
                    const M4 b = blend(mats, ids, w);
                    position = point(b, position);
                    if (hasNormal) normal = direction(b, normal);
                } else if (hasNormal) {
                    normal = unit(normal);
                }
                out.position.add(distance(position, result.positions[v]), v);
                if (hasNormal) out.normal.add(distance(normal, result.normals[v]), v);
                out.moved = std::max(out.moved, distance(result.positions[v], source.positions[v]));
                out.vertices++;
            }
        }
        return out;
    }

    // ---------------------------------------------------------------- a generated glTF

    enum class MorphMode {
        PositionNormalTangent, // 2 targets with every attribute: the shader reads 2
        PositionOnly,          // 3 position targets: the shader reads 8 (5 unbound)
        PositionNormal         // 3 targets of positions and normals: the shader reads 4
    };

    // 8 vertices in 4 rows up +Y, three joints Root (y 0) > Mid (y 1) > Tip (y 2)
    struct Layout {
        std::vector<Vector3> positions, normals;
        std::vector<Vector4> tangents;
        std::vector<Vector2> uv0;
        std::vector<std::array<uint16_t, 2>> uv1;
        std::vector<std::array<uint8_t, 4>> colors, joints, weights;
        std::vector<std::vector<Vector3>> morphPositions, morphNormals, morphTangents;
        std::vector<uint8_t> indices0;   // submesh A: rows 0-2, unsigned byte indices
        std::vector<uint32_t> indices1;  // submesh B: rows 1-3, 32-bit indices
        float jointY[3] = {0.0f, 1.0f, 2.0f};
    };

    Layout layoutData(MorphMode mode) {
        Layout d;
        for (int v = 0; v < 8; v++) {
            const int row = v / 2, col = v % 2;
            d.positions.push_back(Vector3(col - 0.5f, (float)row, 0.05f * v));
            d.normals.push_back(Vector3(0.1f * col, 0.05f * row, 1.0f)); // not unit: the shader normalises
            d.tangents.push_back(Vector4(1.0f, 0.1f * row, 0.0f, (v % 3 == 0) ? -1.0f : 1.0f));
            d.uv0.push_back(Vector2((float)col, row / 3.0f));
            d.uv1.push_back({{(uint16_t)(v * 1000), (uint16_t)(65535 - v * 1000)}});
            d.colors.push_back({{(uint8_t)(v * 30), (uint8_t)(255 - v * 30), 128, (uint8_t)(200 + v)}});
        }
        d.joints = {{{0, 0, 0, 0}}, {{0, 0, 0, 0}}, {{0, 1, 0, 0}}, {{0, 1, 0, 0}},
                    {{0, 1, 2, 0}}, {{0, 1, 2, 0}}, {{2, 0, 0, 0}}, {{2, 0, 0, 0}}};
        d.weights = {{{255, 0, 0, 0}}, {{255, 0, 0, 0}}, {{128, 127, 0, 0}}, {{128, 127, 0, 0}},
                     {{51, 102, 102, 0}}, {{51, 102, 102, 0}}, {{255, 0, 0, 0}}, {{255, 0, 0, 0}}};
        const int targets = (mode == MorphMode::PositionNormalTangent) ? 2 : 3;
        for (int t = 0; t < targets; t++) {
            std::vector<Vector3> p, n, tg;
            for (int v = 0; v < 8; v++) {
                p.push_back(Vector3(0.01f * (t + 1) * v, 0.02f * (t + 1), -0.015f * t * v));
                n.push_back(Vector3(0.05f * (t + 1), 0.0f, -0.02f * v));
                tg.push_back(Vector3(0.0f, 0.03f * (t + 1), 0.01f * v));
            }
            d.morphPositions.push_back(p);
            if (mode != MorphMode::PositionOnly) d.morphNormals.push_back(n);
            if (mode == MorphMode::PositionNormalTangent) d.morphTangents.push_back(tg);
        }
        d.indices0 = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
        d.indices1 = {2, 3, 5, 2, 5, 4, 4, 5, 7, 4, 7, 6};
        return d;
    }

    // `assetsRelative` (+ .bin): the layout in one interleaved view (stride 64: POSITION f3,
    // NORMAL f3, TANGENT f4, TEXCOORD_0 f2, TEXCOORD_1 normalized u16x2, COLOR_0 normalized u8x4,
    // JOINTS_0 u8x4, WEIGHTS_0 normalized u8x4), the morph targets in views of their own.
    // badNormal: NORMAL is a normalized BYTE VEC3 (a format the renderer can't bind)
    void writeLayout(const std::string& assetsRelative, const Layout& d, bool badNormal = false) {
        using nlohmann::json;
        std::vector<unsigned char> bin;
        auto put = [&bin](const void* data, size_t size) {
            const size_t at = bin.size();
            bin.resize(at + size);
            std::memcpy(bin.data() + at, data, size);
            while (bin.size() % 4) bin.push_back(0);
            return at;
        };
        json views = json::array(), accessors = json::array();
        auto view = [&](size_t offset, size_t length, int stride, int target) {
            json v = {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}};
            if (stride) v["byteStride"] = stride;
            if (target) v["target"] = target;
            views.push_back(v);
            return (int)views.size() - 1;
        };
        auto accessor = [&](int viewIndex, size_t offset, int componentType, const char* type, bool normalized) {
            json a = {{"bufferView", viewIndex}, {"byteOffset", offset}, {"componentType", componentType}, {"count", 8}, {"type", type}};
            if (normalized) a["normalized"] = true;
            accessors.push_back(a);
            return (int)accessors.size() - 1;
        };
        auto bounds = [&](int index, const std::vector<Vector3>& values) {
            Vector3 lo = values[0], hi = values[0];
            for (const Vector3& v : values) {
                lo = Vector3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
                hi = Vector3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
            }
            accessors[static_cast<size_t>(index)]["min"] = {lo.x, lo.y, lo.z};
            accessors[static_cast<size_t>(index)]["max"] = {hi.x, hi.y, hi.z};
        };

        std::vector<unsigned char> interleaved(8 * 64, 0);
        for (int v = 0; v < 8; v++) {
            unsigned char* p = interleaved.data() + v * 64;
            const float position[3] = {d.positions[v].x, d.positions[v].y, d.positions[v].z};
            const float normal[3] = {d.normals[v].x, d.normals[v].y, d.normals[v].z};
            const float tangent[4] = {d.tangents[v].x, d.tangents[v].y, d.tangents[v].z, d.tangents[v].w};
            const float uv0[2] = {d.uv0[v].x, d.uv0[v].y};
            std::memcpy(p, position, 12);
            std::memcpy(p + 12, normal, 12);
            std::memcpy(p + 24, tangent, 16);
            std::memcpy(p + 40, uv0, 8);
            std::memcpy(p + 48, d.uv1[v].data(), 4);
            std::memcpy(p + 52, d.colors[v].data(), 4);
            std::memcpy(p + 56, d.joints[v].data(), 4);
            std::memcpy(p + 60, d.weights[v].data(), 4);
        }
        const int vertexView = view(put(interleaved.data(), interleaved.size()), interleaved.size(), 64, 34962);
        json attributes = {
            {"POSITION", accessor(vertexView, 0, 5126, "VEC3", false)},
            {"TANGENT", accessor(vertexView, 24, 5126, "VEC4", false)},
            {"TEXCOORD_0", accessor(vertexView, 40, 5126, "VEC2", false)},
            {"TEXCOORD_1", accessor(vertexView, 48, 5123, "VEC2", true)},
            {"COLOR_0", accessor(vertexView, 52, 5121, "VEC4", true)},
            {"JOINTS_0", accessor(vertexView, 56, 5121, "VEC4", false)},
            {"WEIGHTS_0", accessor(vertexView, 60, 5121, "VEC4", true)}};
        bounds(attributes["POSITION"].get<int>(), d.positions);
        if (badNormal) {
            std::vector<int8_t> bytes(8 * 4, 0);
            for (int v = 0; v < 8; v++) bytes[static_cast<size_t>(v) * 4 + 2] = 127;
            attributes["NORMAL"] = accessor(view(put(bytes.data(), bytes.size()), bytes.size(), 4, 34962), 0, 5120, "VEC3", true);
        } else {
            attributes["NORMAL"] = accessor(vertexView, 12, 5126, "VEC3", false);
        }

        json targets = json::array();
        auto vec3View = [&](const std::vector<Vector3>& values) {
            std::vector<float> flat;
            for (const Vector3& v : values) {
                flat.push_back(v.x);
                flat.push_back(v.y);
                flat.push_back(v.z);
            }
            const int index = accessor(view(put(flat.data(), flat.size() * 4), flat.size() * 4, 0, 34962), 0, 5126, "VEC3", false);
            return index;
        };
        for (size_t t = 0; t < d.morphPositions.size(); t++) {
            json target;
            target["POSITION"] = vec3View(d.morphPositions[t]);
            bounds(target["POSITION"].get<int>(), d.morphPositions[t]);
            if (t < d.morphNormals.size()) target["NORMAL"] = vec3View(d.morphNormals[t]);
            if (t < d.morphTangents.size()) target["TANGENT"] = vec3View(d.morphTangents[t]);
            targets.push_back(target);
        }

        const int index0 = view(put(d.indices0.data(), d.indices0.size()), d.indices0.size(), 0, 34963);
        accessors.push_back({{"bufferView", index0}, {"componentType", 5121}, {"count", d.indices0.size()}, {"type", "SCALAR"}});
        const int indices0 = (int)accessors.size() - 1;
        const int index1 = view(put(d.indices1.data(), d.indices1.size() * 4), d.indices1.size() * 4, 0, 34963);
        accessors.push_back({{"bufferView", index1}, {"componentType", 5125}, {"count", d.indices1.size()}, {"type", "SCALAR"}});
        const int indices1 = (int)accessors.size() - 1;

        float ibm[48];
        for (int j = 0; j < 3; j++) {
            for (int i = 0; i < 16; i++) ibm[j * 16 + i] = (i % 5 == 0) ? 1.0f : 0.0f;
            ibm[j * 16 + 13] = -d.jointY[j];
        }
        const int ibmView = view(put(ibm, sizeof(ibm)), sizeof(ibm), 0, 0);
        accessors.push_back({{"bufferView", ibmView}, {"componentType", 5126}, {"count", 3}, {"type", "MAT4"}});
        const int ibmAccessor = (int)accessors.size() - 1;

        json primitive0 = {{"attributes", attributes}, {"indices", indices0}, {"material", 0}};
        json primitive1 = {{"attributes", attributes}, {"indices", indices1}, {"material", 1}};
        json names = json::array();
        json defaults = json::array();
        if (!targets.empty()) {
            primitive0["targets"] = targets;
            primitive1["targets"] = targets;
            for (size_t t = 0; t < targets.size(); t++) {
                names.push_back("Shape" + std::to_string(t));
                defaults.push_back(0.0);
            }
        }
        json mesh = {{"name", "Layout"}, {"primitives", {primitive0, primitive1}}};
        if (!targets.empty()) {
            mesh["weights"] = defaults;
            mesh["extras"] = {{"targetNames", names}};
        }
        const fs::path path = project()->getAssetsPath() / assetsRelative;
        fs::create_directories(path.parent_path());
        const std::string binName = path.stem().string() + ".bin";
        json gltf = {
            {"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", {{{"nodes", {0, 1}}}}},
            {"nodes", {
                {{"name", "Layout"}, {"mesh", 0}, {"skin", 0}},
                {{"name", "Root"}, {"children", {2}}},
                {{"name", "Mid"}, {"translation", {0.0, d.jointY[1], 0.0}}, {"children", {3}}},
                {{"name", "Tip"}, {"translation", {0.0, d.jointY[2] - d.jointY[1], 0.0}}}}},
            {"skins", {{{"joints", {1, 2, 3}}, {"inverseBindMatrices", ibmAccessor}, {"skeleton", 1}}}},
            {"meshes", json::array({mesh})},
            {"materials", {{{"name", "A"}}, {{"name", "B"}}}},
            {"buffers", {{{"uri", binName}, {"byteLength", bin.size()}}}},
            {"bufferViews", views},
            {"accessors", accessors}};
        std::ofstream(path.parent_path() / binName, std::ios::binary).write(reinterpret_cast<const char*>(bin.data()),
                                                                            static_cast<std::streamsize>(bin.size()));
        std::ofstream(path) << gltf.dump(2);
    }

    // The generated layout by plain math from its arrays: per buffer index, mesh space
    struct LayoutReference {
        std::vector<Vector3> positions, normals;
        std::vector<Vector4> tangents;
    };

    LayoutReference layoutReference(Scene* scene, Entity model, const Layout& d, const float* morphWeights) {
        const ModelComponent& component = scene->getComponent<ModelComponent>(model);
        const M4 inverseMesh = inverse(worldOf(scene, model));
        std::vector<M4> mats;
        for (int j = 0; j < 3; j++) {
            M4 ibm = identity();
            ibm.m[3][1] = -d.jointY[j];
            mats.push_back(mul(mul(inverseMesh, worldOf(scene, jointEntity(component, j + 1))), ibm));
        }
        LayoutReference out;
        for (int v = 0; v < 8; v++) {
            Vector3 p = d.positions[v], n = d.normals[v];
            Vector3 t(d.tangents[v].x, d.tangents[v].y, d.tangents[v].z);
            for (size_t k = 0; k < d.morphPositions.size(); k++) {
                p = p + d.morphPositions[k][v] * morphWeights[k];
                if (k < d.morphNormals.size()) n = n + d.morphNormals[k][v] * morphWeights[k];
                if (k < d.morphTangents.size()) t = t + d.morphTangents[k][v] * morphWeights[k];
            }
            int ids[4];
            double w[4];
            for (int k = 0; k < 4; k++) {
                ids[k] = d.joints[v][k];
                w[k] = d.weights[v][k] / 255.0;
            }
            const M4 b = blend(mats, ids, w);
            out.positions.push_back(point(b, p));
            out.normals.push_back(direction(b, n));
            const Vector3 tangent = direction(b, t);
            out.tangents.push_back(Vector4(tangent.x, tangent.y, tangent.z, d.tangents[v].w));
        }
        return out;
    }

    // ---------------------------------------------------------------- lattices

    // a lattice entity around [center - size/2, center + size/2] with hashed offsets
    Entity addLattice(Scene* scene, const Vector3& center, const Vector3& size, int rx, int ry, int rz,
                      LatticeInterpolation interpolation, LatticeOutside outside, float salt, float amount) {
        const Entity entity = scene->createUserEntity();
        scene->addComponent<Transform>(entity, {});
        Transform& transform = scene->getComponent<Transform>(entity);
        transform.position = center;
        transform.scale = size;
        transform.needUpdate = true;
        LatticeComponent lattice;
        lattice.resolutionX = rx;
        lattice.resolutionY = ry;
        lattice.resolutionZ = rz;
        lattice.interpolation = interpolation;
        lattice.outside = outside;
        lattice.offsets.assign(static_cast<size_t>(rx * ry * rz), Vector3(0, 0, 0));
        for (size_t p = 0; p < lattice.offsets.size(); p++) {
            const float f = (float)p + salt;
            lattice.offsets[p] = Vector3(std::sin(f * 1.3f), std::cos(f * 0.7f), std::sin(f * 2.1f + 0.4f)) * amount;
        }
        scene->addComponent<LatticeComponent>(entity, lattice);
        return entity;
    }

    void bindLattices(Scene* scene, Entity mesh, const std::vector<LatticeBinding>& bindings) {
        LatticeModifierComponent modifier;
        modifier.bindings = bindings;
        scene->addComponent<LatticeModifierComponent>(mesh, modifier);
    }

    LatticeBinding binding(Entity lattice, LatticeStage stage, float weight, int submesh = -1,
                           LatticeMaskChannel mask = LatticeMaskChannel::NONE) {
        LatticeBinding b;
        b.lattice = lattice;
        b.stage = stage;
        b.weight = weight;
        b.submesh = submesh;
        b.mask = mask;
        return b;
    }

    // lattice::deformVertex's input straight from the components (not from the packed records)
    struct DirectLattice {
        LatticeComponent component;
        std::vector<Vector3> padded;
        lattice::ObjectSpaceLattice binding;
    };

    std::unique_ptr<DirectLattice> directLattice(Scene* scene, Entity latticeEntity, Entity meshEntity, float weight,
                                                 LatticeMaskChannel mask = LatticeMaskChannel::NONE) {
        auto out = std::make_unique<DirectLattice>();
        out->component = scene->getComponent<LatticeComponent>(latticeEntity);
        lattice::buildPaddedGrid(out->component, out->padded);
        out->binding.lattice = &out->component;
        out->binding.padded = &out->padded;
        out->binding.objectToNormalized = lattice::objectToNormalized(scene->getComponent<Transform>(latticeEntity).modelMatrix,
                                                                      scene->getComponent<Transform>(meshEntity).modelMatrix);
        out->binding.normalizedToObject = out->binding.objectToNormalized.inverse();
        out->binding.weight = weight;
        out->binding.mask = mask;
        return out;
    }

    void bounds(const std::vector<Vector3>& points, Vector3& center, Vector3& size) {
        Vector3 lo = points[0], hi = points[0];
        for (const Vector3& p : points) {
            lo = Vector3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vector3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        center = (lo + hi) * 0.5f;
        size = Vector3(std::max(hi.x - lo.x, 0.2f), std::max(hi.y - lo.y, 0.2f), std::max(hi.z - lo.z, 0.2f)) * 1.1f;
    }

    // ---------------------------------------------------------------- the fixture

    class CpuSkin_ : public ::testing::Test {
    protected:
        void SetUp() override {
            ASSERT_TRUE(headless().ready) << "the headless project did not open";
        }
    };

    std::shared_ptr<const AnimationClip> modelClip(const std::string& reference) {
        ClipData data;
        std::vector<ClipWarning> warnings;
        std::string error;
        EXPECT_TRUE(ClipConverters::extractModelSubClip(reference, data, warnings, &error)) << error;
        return AnimationClip::build(data);
    }

    void poseAt(Scene* scene, Entity root, const std::shared_ptr<const AnimationClip>& clip, float time, anim::ClipPreviewSession& session) {
        ActionSystem::PlayerPreview preview;
        ActionSystem::PlayerPreviewState state;
        state.clip = clip;
        state.time = time;
        preview.states.push_back(state);
        ActionSystem::PlayerPreviewResult result = scene->getSystem<ActionSystem>()->previewPlayer(root, preview, session);
        EXPECT_TRUE(result.ready);
        EXPECT_TRUE(result.unresolved.empty());
        updateTransforms(scene);
    }

    // every bone of the model at its BoneComponent bind TRS
    void bindPose(Scene* scene, Entity model) {
        for (const auto& bone : scene->getComponent<ModelComponent>(model).bonesIdMapping) {
            const BoneComponent* component = scene->findComponent<BoneComponent>(bone.second);
            Transform* transform = scene->findComponent<Transform>(bone.second);
            if (!component || !transform) continue;
            transform->position = component->bindPosition;
            transform->rotation = component->bindRotation;
            transform->scale = component->bindScale;
        }
        updateTransforms(scene);
    }

}

// ==================================================================== the source

TEST_F(CpuSkin_, HeroSourceReadsEveryVertexOfEverySubmeshAsTheGpu) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    const Entity meshEntity = meshEntityOf(scene, hero);
    ASSERT_NE(meshEntity, NULL_ENTITY);
    ASSERT_NE(meshEntity, hero) << "the animated hero's geometry is on its mesh node";

    CpuSkinSource source;
    std::string error;
    ASSERT_TRUE(buildSource(scene, meshEntity, source, &error)) << error;
    const tinygltf::Model& g = *scene->getComponent<ModelComponent>(hero).gltfModel;
    const tinygltf::Primitive& first = g.meshes[0].primitives[0];
    const std::vector<double> positions = readAccessor(g, first.attributes.at("POSITION"));
    const std::vector<double> joints = readAccessor(g, first.attributes.at("JOINTS_0"));
    const std::vector<double> weights = readAccessor(g, first.attributes.at("WEIGHTS_0"));

    ASSERT_EQ(source.submeshes.size(), 3u);
    size_t expectedVertices = 0;
    std::map<uint32_t, uint32_t> firstVertexOfIndex;
    for (const CpuSkinSubmesh& submesh : source.submeshes) {
        EXPECT_TRUE(submesh.hasSkinning);
        EXPECT_TRUE(submesh.hasNormal);
        EXPECT_FALSE(submesh.hasTangent);
        EXPECT_EQ(submesh.morphTargets, 0);
        EXPECT_EQ(submesh.normAdjust.x, 65535.0f) << "unsigned short joints bind normalized";
        EXPECT_EQ(submesh.normAdjust.y, 1.0f);
        EXPECT_EQ(submesh.firstVertex, expectedVertices) << "submeshes are contiguous";
        // every vertex its indices reference, once, ascending
        std::vector<uint32_t> referenced;
        for (uint32_t index : submesh.indices) referenced.push_back(source.vertexIndex[index]);
        std::sort(referenced.begin(), referenced.end());
        referenced.erase(std::unique(referenced.begin(), referenced.end()), referenced.end());
        ASSERT_EQ(referenced.size(), submesh.vertexCount);
        for (uint32_t k = 0; k < submesh.vertexCount; k++) {
            const uint32_t v = submesh.firstVertex + k;
            EXPECT_EQ(source.vertexIndex[v], referenced[k]);
            EXPECT_EQ(source.vertexSubmesh[v], submesh.submesh);
            EXPECT_EQ(&source.submeshOf(v), &submesh);
            const uint32_t e = source.vertexIndex[v];
            EXPECT_EQ(source.positions[v].x, (float)positions[e * 3]);
            EXPECT_EQ(source.positions[v].y, (float)positions[e * 3 + 1]);
            EXPECT_EQ(source.positions[v].z, (float)positions[e * 3 + 2]);
            for (int c = 0; c < 4; c++) {
                // int(id / 65535 * 65535) is the id again
                EXPECT_EQ(source.boneIds[v][c], (int)joints[e * 4 + static_cast<size_t>(c)]);
            }
            EXPECT_EQ(source.boneWeights[v].x, (float)weights[e * 4]);
            EXPECT_EQ(source.boneWeights[v].w, (float)weights[e * 4 + 3]);
            // the three submeshes bind the same accessors: a vertex used by several is the first's
            auto known = firstVertexOfIndex.emplace(e, v);
            EXPECT_EQ(source.canonical[v], known.first->second);
        }
        expectedVertices += submesh.vertexCount;
    }
    EXPECT_EQ(source.vertexCount(), expectedVertices);
    // the vertices the primitives draw, by their index accessors: no more (the accessors' 2801
    // include two no triangle uses), no fewer, each once per submesh drawing it
    std::map<uint32_t, int> drawnBy;
    for (const tinygltf::Primitive& primitive : g.meshes[0].primitives) {
        std::vector<double> indices = readAccessor(g, primitive.indices);
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        for (double index : indices) drawnBy[(uint32_t)index]++;
    }
    size_t drawnTotal = 0;
    for (const auto& entry : drawnBy) drawnTotal += static_cast<size_t>(entry.second);
    EXPECT_EQ(firstVertexOfIndex.size(), drawnBy.size());
    EXPECT_EQ(source.vertexCount(), drawnTotal);
    EXPECT_LT(firstVertexOfIndex.size(), 2801u) << "vertices no triangle draws are left out";
}

// ==================================================================== rest and clip poses

TEST_F(CpuSkin_, BindPoseGivesTheBindPositions) {
    for (const char* model : {"models/hero.gltf", "models/tpose.gltf"}) {
        SCOPED_TRACE(model);
        const uint32_t sceneId = newScene();
        Scene* scene = sceneOf(sceneId);
        const Entity hero = loadModel(sceneId, model);
        const Entity meshEntity = meshEntityOf(scene, hero);
        bindPose(scene, hero);

        CpuSkinSource source;
        CpuSkinResult result;
        std::string error;
        ASSERT_TRUE(deformEntity(scene, meshEntity, result, &error, &source)) << error;
        MaxError position, normal;
        for (size_t v = 0; v < source.vertexCount(); v++) {
            position.add(distance(result.positions[v], source.positions[v]), v);
            normal.add(distance(result.normals[v], unit(source.normals[v])), v);
        }
        EXPECT_LT(position.value, 1e-4) << "vertex " << position.at;
        EXPECT_LT(normal.value, 1e-4) << "vertex " << normal.at;
    }
}

TEST_F(CpuSkin_, ClipPosesMatchThePlainMathReference) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    const Entity meshEntity = meshEntityOf(scene, hero);
    // the model placed somewhere: mesh space is the mesh entity's
    Transform& placement = scene->getComponent<Transform>(hero);
    placement.position = Vector3(2.0f, -1.0f, 3.5f);
    placement.rotation = axisAngle(70.0, 0.2, 1.0, 0.1);
    placement.scale = Vector3(1.3f, 1.3f, 1.3f);

    anim::ClipPreviewSession session;
    for (const char* reference : {"models/hero.gltf#Walk", "models/hero.gltf#Wave"}) {
        const std::shared_ptr<const AnimationClip> clip = modelClip(reference);
        ASSERT_TRUE(clip);
        for (float fraction : {0.0f, 0.17f, 0.41f, 0.66f, 0.93f}) {
            const float time = clip->getLength() * fraction;
            SCOPED_TRACE(std::string(reference) + " at " + std::to_string(time));
            poseAt(scene, hero, clip, time, session);

            // updateTransforms made every modelMatrix the TRS chain's
            const ModelComponent& heroModel = scene->getComponent<ModelComponent>(hero);
            for (Entity e : {hero, meshEntity, jointEntity(heroModel, heroModel.gltfModel->skins[0].joints[5])}) {
                const M4 expected = worldOf(scene, e);
                const Matrix4& actual = scene->getComponent<Transform>(e).modelMatrix;
                for (int c = 0; c < 4; c++)
                    for (int r = 0; r < 4; r++) EXPECT_NEAR(actual.get(c, r), expected.m[c][r], 1e-4);
            }

            CpuSkinSource source;
            CpuSkinResult result;
            std::string error;
            ASSERT_TRUE(deformEntity(scene, meshEntity, result, &error, &source)) << error;
            const GltfComparison compared = compareWithGltf(scene, hero, meshEntity, source, result);
            EXPECT_EQ(compared.vertices, source.vertexCount());
            EXPECT_LT(compared.position.value, 1e-4) << "vertex " << compared.position.at;
            EXPECT_LT(compared.normal.value, 1e-4) << "vertex " << compared.normal.at;
            if (fraction > 0.0f) {
                EXPECT_GT(compared.moved, 0.01) << "the clip poses the mesh";
            }
            // shared vertices deform alike
            for (size_t v = 0; v < source.vertexCount(); v++) {
                EXPECT_EQ(result.positions[v], result.positions[source.canonical[v]]);
            }
        }
    }
    scene->getSystem<ActionSystem>()->restorePlayerPreview(session);
}

TEST_F(CpuSkin_, SkinMatricesAreTheRenderFormulaOnBothPaths) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    // alone, the T-pose hero loads in the legacy layout: the mesh on the model entity
    const Entity hero = loadModel(sceneId, "models/tpose.gltf");
    ModelComponent& model = scene->getComponent<ModelComponent>(hero);
    ASSERT_TRUE(model.nodesIdMapping.empty()) << "legacy layout";
    ASSERT_EQ(meshEntityOf(scene, hero), hero);
    ASSERT_FALSE(model.skinBindings.empty());
    scene->getComponent<Transform>(hero).position = Vector3(-1.0f, 0.5f, 0.0f);
    scene->getComponent<Transform>(hero).rotation = axisAngle(-35.0, 0.0, 1.0, 0.0);
    int turned = 0;
    for (const auto& bone : model.bonesIdMapping) {
        Transform& transform = scene->getComponent<Transform>(bone.second);
        transform.rotation = transform.rotation * axisAngle(12.0 + 3.0 * (turned % 5), 1.0, 0.3 * (turned % 3), -0.2);
        turned++;
    }
    updateTransforms(scene);

    std::vector<Matrix4> skin;
    std::string error;
    ASSERT_TRUE(computeSkinMatrices(scene, hero, skin, &error)) << error;
    const ModelSkinBinding& binding = model.skinBindings[0];
    ASSERT_GE(skin.size(), binding.joints.size());
    const M4 inverseMesh = inverse(worldOf(scene, hero));
    for (size_t j = 0; j < binding.joints.size(); j++) {
        const M4 expected = mul(mul(inverseMesh, worldOf(scene, binding.joints[j])), fromMatrix(binding.inverseBindMatrices[j]));
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) EXPECT_NEAR(skin[j].get(c, r), expected.m[c][r], 1e-4) << "joint " << j;
    }

    // a model without skin bindings: the BoneComponent path gives the same
    const std::vector<ModelSkinBinding> saved = model.skinBindings;
    model.skinBindings.clear();
    std::vector<Matrix4> legacy;
    ASSERT_TRUE(computeSkinMatrices(scene, hero, legacy, &error)) << error;
    model.skinBindings = saved;
    ASSERT_EQ(legacy.size(), skin.size());
    for (size_t j = 0; j < skin.size(); j++)
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) EXPECT_NEAR(legacy[j].get(c, r), skin[j].get(c, r), 1e-5) << "bone " << j;

    // and the whole mesh matches the plain math
    CpuSkinSource source;
    CpuSkinResult result;
    ASSERT_TRUE(deformEntity(scene, hero, result, &error, &source)) << error;
    const GltfComparison compared = compareWithGltf(scene, hero, hero, source, result);
    EXPECT_LT(compared.position.value, 1e-4) << "vertex " << compared.position.at;
    EXPECT_LT(compared.normal.value, 1e-4) << "vertex " << compared.normal.at;
    EXPECT_GT(compared.moved, 0.05);

    // a mesh scaled to 0 has no mesh space to skin into
    scene->getComponent<Transform>(hero).scale = Vector3(0.0f, 0.0f, 0.0f);
    updateTransforms(scene);
    EXPECT_FALSE(computeSkinMatrices(scene, hero, skin, &error));
    EXPECT_NE(error.find("not invertible"), std::string::npos) << error;
}

TEST_F(CpuSkin_, AgreesWithPoseSkinOnItsSubsample) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    const Entity meshEntity = meshEntityOf(scene, hero);
    const std::shared_ptr<const AnimationClip> clip = modelClip("models/hero.gltf#Walk");
    anim::ClipPreviewSession session;
    poseAt(scene, hero, clip, clip->getLength() * 0.3f, session);

    pose::Skeleton skeleton;
    std::vector<Entity> boneEntities;
    ASSERT_TRUE(pose::buildSkeleton(scene, hero, skeleton, boneEntities));
    const std::shared_ptr<pose::PoseSkin> poseSkin = pose::buildSkin(scene, hero, skeleton, boneEntities);
    ASSERT_TRUE(poseSkin);
    ASSERT_GT(poseSkin->vertices.size(), 1000u);
    // the bones in model space (PoseRig::computeWorldMatrices' space)
    const Matrix4 inverseModel = scene->getComponent<Transform>(hero).modelMatrix.inverse();
    std::vector<Matrix4> world;
    for (Entity bone : boneEntities) world.push_back(inverseModel * scene->getComponent<Transform>(bone).modelMatrix);

    CpuSkinSource source;
    CpuSkinResult result;
    std::string error;
    ASSERT_TRUE(deformEntity(scene, meshEntity, result, &error, &source)) << error;
    const Matrix4 meshToModel = inverseModel * scene->getComponent<Transform>(meshEntity).modelMatrix;
    // CpuSkin's vertices by bind position
    std::multimap<std::array<float, 3>, uint32_t> byPosition;
    for (uint32_t v = 0; v < source.vertexCount(); v++) {
        if (source.canonical[v] == v) byPosition.emplace(std::array<float, 3>{{source.positions[v].x, source.positions[v].y, source.positions[v].z}}, v);
    }

    MaxError worst;
    float lowestSample = INFINITY;
    for (size_t s = 0; s < poseSkin->vertices.size(); s++) {
        pose::PoseSkin one;
        one.inverseBind = poseSkin->inverseBind;
        one.vertices = {poseSkin->vertices[s]};
        const float y = one.lowest(world);
        lowestSample = std::min(lowestSample, y);
        const Vector3& p = poseSkin->vertices[s].position;
        auto range = byPosition.equal_range({{p.x, p.y, p.z}});
        ASSERT_NE(range.first, range.second) << "sample " << s << " is a vertex of the mesh";
        double best = INFINITY;
        for (auto it = range.first; it != range.second; ++it) best = std::min(best, (double)std::fabs((meshToModel * result.positions[it->second]).y - y));
        worst.add(best, s);
    }
    EXPECT_LT(worst.value, 1e-4) << "sample " << worst.at;
    EXPECT_NEAR(poseSkin->lowest(world), lowestSample, 1e-6);
    scene->getSystem<ActionSystem>()->restorePlayerPreview(session);
}

// ==================================================================== morph targets

TEST_F(CpuSkin_, MorphTargetsAreAppliedBeforeSkinning) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity strip = loadModel(sceneId, "models/strip.gltf");
    const Entity meshEntity = meshEntityOf(scene, strip);
    ModelComponent& model = scene->getComponent<ModelComponent>(strip);
    const tinygltf::Model& g = *model.gltfModel;
    const int tipNode = g.skins[0].joints[1];
    Transform& tip = scene->getComponent<Transform>(jointEntity(model, tipNode));
    tip.rotation = tip.rotation * axisAngle(35.0, 0.0, 0.0, 1.0);
    updateTransforms(scene);

    MeshComponent& mesh = scene->getComponent<MeshComponent>(meshEntity);
    CpuSkinSource source;
    CpuSkinResult without, with;
    std::string error;
    mesh.morphWeights[0] = 0.0f;
    ASSERT_TRUE(deformEntity(scene, meshEntity, without, &error, &source)) << error;
    ASSERT_EQ(source.submeshes.size(), 1u);
    EXPECT_EQ(source.submeshes[0].morphTargets, MAX_MORPHTARGETS) << "positions only: the shader reads 8 targets";
    EXPECT_EQ(source.submeshes[0].morphPositionMask, 1u);
    EXPECT_EQ(source.submeshes[0].morphNormalTargets, 0);
    EXPECT_FALSE(source.morphPositions[0].empty());
    EXPECT_TRUE(source.morphPositions[1].empty());

    mesh.morphWeights[0] = 0.8f;
    ASSERT_TRUE(deformEntity(scene, meshEntity, with, &error)) << error;
    const GltfComparison compared = compareWithGltf(scene, strip, meshEntity, source, with);
    EXPECT_LT(compared.position.value, 1e-4) << "vertex " << compared.position.at;
    EXPECT_LT(compared.normal.value, 1e-4) << "vertex " << compared.normal.at;
    double changed = 0.0;
    for (size_t v = 0; v < source.vertexCount(); v++) changed = std::max(changed, distance(with.positions[v], without.positions[v]));
    EXPECT_GT(changed, 0.01) << "the weight moves the vertices";

    // cpuDeform with no weights (null) is the weights at 0
    std::vector<Matrix4> skin;
    ASSERT_TRUE(computeSkinMatrices(scene, meshEntity, skin, &error)) << error;
    CpuSkinResult none;
    ASSERT_TRUE(cpuDeform(source, skin, nullptr, nullptr, none, &error)) << error;
    for (size_t v = 0; v < source.vertexCount(); v++) EXPECT_EQ(none.positions[v], without.positions[v]);
}

// The generated glTF: one interleaved view (stride 64) of normalized integer attributes,
// unsigned byte joints, byte and 32-bit indices, two submeshes sharing four vertices, morph
// targets of positions, normals and tangents, tangents of either handedness
TEST_F(CpuSkin_, InterleavedNormalizedLayoutWithMorphsAndTangents) {
    struct Case {
        const char* file;
        MorphMode mode;
        int positions, normals, tangents;
    };
    for (const Case& c : {Case{"models/gen/layout.gltf", MorphMode::PositionNormalTangent, 2, 2, 2},
                          Case{"models/gen/layout_pos.gltf", MorphMode::PositionOnly, 8, 0, 0},
                          Case{"models/gen/layout_nrm.gltf", MorphMode::PositionNormal, 4, 4, 0}}) {
        SCOPED_TRACE(c.file);
        const Layout d = layoutData(c.mode);
        writeLayout(c.file, d);
        const uint32_t sceneId = newScene();
        Scene* scene = sceneOf(sceneId);
        const Entity model = loadModel(sceneId, c.file);
        ASSERT_EQ(meshEntityOf(scene, model), model) << "one skinned mesh node: the legacy layout";

        CpuSkinSource source;
        std::string error;
        ASSERT_TRUE(buildSource(scene, model, source, &error)) << error;
        ASSERT_EQ(source.submeshes.size(), 2u);
        // the submesh drawing the byte indices (as 16-bit) and the one drawing the 32-bit ones
        for (const CpuSkinSubmesh& submesh : source.submeshes) {
            std::vector<uint32_t> drawn;
            for (uint32_t index : submesh.indices) drawn.push_back(source.vertexIndex[index]);
            const bool first = drawn == std::vector<uint32_t>(d.indices0.begin(), d.indices0.end());
            const bool second = drawn == d.indices1;
            EXPECT_TRUE(first || second);
            EXPECT_EQ(submesh.vertexCount, 6u);
            EXPECT_TRUE(submesh.hasNormal && submesh.hasTangent && submesh.hasUv0 && submesh.hasUv1 && submesh.hasSkinning);
            EXPECT_EQ(submesh.colorComponents, 4);
            EXPECT_EQ(submesh.normAdjust.x, 255.0f);
            EXPECT_EQ(submesh.normAdjust.y, 255.0f);
            EXPECT_EQ(submesh.morphTargets, c.positions);
            EXPECT_EQ(submesh.morphNormalTargets, c.normals);
            EXPECT_EQ(submesh.morphTangentTargets, c.tangents);
            EXPECT_EQ(submesh.morphPositionMask, (c.mode == MorphMode::PositionNormalTangent) ? 3u : 7u);
        }
        // buffer indices 2-5 are in both submeshes: the second's copies name the first's
        const CpuSkinSubmesh& a = source.submeshes[0];
        const CpuSkinSubmesh& b = source.submeshes[1];
        int shared = 0;
        for (uint32_t v = b.firstVertex; v < b.firstVertex + b.vertexCount; v++) {
            if (source.canonical[v] != v) {
                shared++;
                EXPECT_LT(source.canonical[v], a.firstVertex + a.vertexCount);
                EXPECT_EQ(source.vertexIndex[source.canonical[v]], source.vertexIndex[v]);
            }
        }
        EXPECT_EQ(shared, 4);

        // the decoded attributes
        for (uint32_t v = 0; v < source.vertexCount(); v++) {
            const uint32_t e = source.vertexIndex[v];
            EXPECT_EQ(source.positions[v], d.positions[e]);
            EXPECT_EQ(source.tangents[v].w, d.tangents[e].w);
            EXPECT_EQ(source.uv0[v].x, d.uv0[e].x);
            EXPECT_EQ(source.uv0[v].y, d.uv0[e].y);
            EXPECT_EQ(source.uv1[v].x, d.uv1[e][0] / 65535.0f);
            EXPECT_EQ(source.uv1[v].y, d.uv1[e][1] / 65535.0f);
            EXPECT_EQ(source.colors[v].x, d.colors[e][0] / 255.0f);
            EXPECT_EQ(source.colors[v].w, d.colors[e][3] / 255.0f);
            for (int k = 0; k < 4; k++) EXPECT_EQ(source.boneIds[v][k], (int)d.joints[e][k]);
            EXPECT_NEAR(source.boneWeights[v].x, (float)d.weights[e][0], 1e-4) << "normalized bytes times normAdjust 255";
            for (size_t t = 0; t < d.morphPositions.size(); t++) EXPECT_EQ(source.morphPositions[t][v], d.morphPositions[t][e]);
            for (size_t t = 0; t < d.morphNormals.size(); t++) EXPECT_EQ(source.morphNormals[t][v], d.morphNormals[t][e]);
            for (size_t t = 0; t < d.morphTangents.size(); t++) EXPECT_EQ(source.morphTangents[t][v], d.morphTangents[t][e]);
        }

        // posed, placed and morphed
        ModelComponent& component = scene->getComponent<ModelComponent>(model);
        Transform& placement = scene->getComponent<Transform>(model);
        placement.position = Vector3(1.0f, 2.0f, 3.0f);
        placement.rotation = axisAngle(30.0, 0.0, 1.0, 0.0);
        placement.scale = Vector3(1.2f, 0.8f, 1.0f);
        scene->getComponent<Transform>(jointEntity(component, 1)).position = Vector3(0.1f, 0.0f, 0.2f);
        Transform& mid = scene->getComponent<Transform>(jointEntity(component, 2));
        mid.rotation = mid.rotation * axisAngle(35.0, 0.0, 0.0, 1.0);
        Transform& tip = scene->getComponent<Transform>(jointEntity(component, 3));
        tip.rotation = tip.rotation * axisAngle(-50.0, 1.0, 0.0, 0.0);
        updateTransforms(scene);
        MeshComponent& mesh = scene->getComponent<MeshComponent>(model);
        mesh.morphWeights[0] = 0.6f;
        mesh.morphWeights[1] = -0.3f;
        mesh.morphWeights[2] = 0.25f;

        CpuSkinResult result;
        ASSERT_TRUE(deformEntity(scene, model, result, &error)) << error;
        const LayoutReference expected = layoutReference(scene, model, d, mesh.morphWeights);
        MaxError position, normal, tangent;
        for (uint32_t v = 0; v < source.vertexCount(); v++) {
            const uint32_t e = source.vertexIndex[v];
            position.add(distance(result.positions[v], expected.positions[e]), v);
            normal.add(distance(result.normals[v], expected.normals[e]), v);
            const Vector3 t(result.tangents[v].x, result.tangents[v].y, result.tangents[v].z);
            tangent.add(distance(t, Vector3(expected.tangents[e].x, expected.tangents[e].y, expected.tangents[e].z)), v);
            EXPECT_EQ(result.tangents[v].w, d.tangents[e].w) << "the tangent keeps its handedness";
            EXPECT_NEAR(t.length(), 1.0f, 1e-5f);
            EXPECT_NEAR(result.normals[v].length(), 1.0f, 1e-5f);
        }
        EXPECT_LT(position.value, 1e-4) << "vertex " << position.at;
        EXPECT_LT(normal.value, 1e-4) << "vertex " << normal.at;
        EXPECT_LT(tangent.value, 1e-4) << "vertex " << tangent.at;
    }
}

// ==================================================================== rigid and interleaved meshes

TEST_F(CpuSkin_, RigidMeshesKeepTheirVertices) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);

    // the procedural box: its own interleaved "vertices" and "indices" buffers, no submesh attributes
    const Entity box = scene->createUserEntity();
    scene->addComponent<Transform>(box, {});
    scene->addComponent<MeshComponent>(box, {});
    MeshComponent& boxMesh = scene->getComponent<MeshComponent>(box);
    scene->getSystem<MeshSystem>()->createBox(boxMesh, 1.0f, 2.0f, 0.5f);
    CpuSkinSource source;
    CpuSkinResult result;
    std::string error;
    ASSERT_TRUE(buildSource(boxMesh, source, &error)) << error;
    ASSERT_EQ(source.submeshes.size(), 1u);
    EXPECT_FALSE(source.submeshes[0].hasSkinning);
    EXPECT_TRUE(source.submeshes[0].hasUv0);
    EXPECT_EQ(source.submeshes[0].colorComponents, 4);
    ASSERT_EQ(source.vertexCount(), (size_t)boxMesh.buffer.getCount());
    ASSERT_EQ(source.submeshes[0].indices.size(), (size_t)boxMesh.indices.getCount());
    for (size_t k = 0; k < source.submeshes[0].indices.size(); k++)
        EXPECT_EQ(source.vertexIndex[source.submeshes[0].indices[k]], boxMesh.indices.getUInt16(AttributeType::INDEX, (unsigned int)k));
    for (uint32_t v = 0; v < source.vertexCount(); v++) {
        EXPECT_EQ(source.positions[v], boxMesh.buffer.getVector3(AttributeType::POSITION, source.vertexIndex[v]));
        EXPECT_EQ(source.normals[v], boxMesh.buffer.getVector3(AttributeType::NORMAL, source.vertexIndex[v]));
    }
    ASSERT_TRUE(cpuDeform(source, {}, nullptr, nullptr, result, &error)) << "no skin: no matrices needed: " << error;
    for (uint32_t v = 0; v < source.vertexCount(); v++) {
        EXPECT_EQ(result.positions[v], source.positions[v]);
        EXPECT_LT(distance(result.normals[v], unit(source.normals[v])), 1e-6);
    }

    // a rigid glTF of three submeshes over the same vertex buffers
    const Entity figure = loadModel(sceneId, "models/figure.gltf");
    ASSERT_TRUE(deformEntity(scene, meshEntityOf(scene, figure), result, &error, &source)) << error;
    ASSERT_EQ(source.submeshes.size(), 3u);
    for (const CpuSkinSubmesh& submesh : source.submeshes) EXPECT_FALSE(submesh.hasSkinning);
    for (uint32_t v = 0; v < source.vertexCount(); v++) EXPECT_EQ(result.positions[v], source.positions[v]);
    const GltfComparison compared = compareWithGltf(scene, figure, meshEntityOf(scene, figure), source, result);
    EXPECT_EQ(compared.vertices, source.vertexCount());
    EXPECT_LT(compared.normal.value, 1e-5);
}

// An interleaved skinned mesh of float bone ids and weights (what a saved mesh can hold): ids
// read as floats with normAdjust 1, an id past the matrices reads a zero matrix
TEST_F(CpuSkin_, InterleavedSkinnedBuffer) {
    MeshComponent mesh;
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::BONEIDS, 4);
    mesh.buffer.addAttribute(AttributeType::BONEWEIGHTS, 4);
    const std::vector<Vector3> positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(1, 1, 1)};
    const std::vector<Vector4> ids = {Vector4(0, 0, 0, 0), Vector4(1, 0, 0, 0), Vector4(0, 1, 0, 0), Vector4(1, 5, 0, 0)};
    const std::vector<Vector4> weights = {Vector4(1, 0, 0, 0), Vector4(1, 0, 0, 0), Vector4(0.25f, 0.75f, 0, 0), Vector4(0.5f, 0.5f, 0, 0)};
    for (size_t v = 0; v < positions.size(); v++) {
        mesh.buffer.addVector3(AttributeType::POSITION, positions[v]);
        mesh.buffer.addVector3(AttributeType::NORMAL, Vector3(0, 0, 2));
        mesh.buffer.addVector4(AttributeType::BONEIDS, ids[v]);
        mesh.buffer.addVector4(AttributeType::BONEWEIGHTS, weights[v]);
    }
    for (uint16_t i : {0, 1, 2, 1, 3, 2}) mesh.indices.addUInt16(AttributeType::INDEX, i);
    mesh.numSubmeshes = 1;
    mesh.vertexCount = 4;

    CpuSkinSource source;
    std::string error;
    ASSERT_TRUE(buildSource(mesh, source, &error)) << error;
    ASSERT_TRUE(source.submeshes[0].hasSkinning);
    EXPECT_EQ(source.submeshes[0].normAdjust.x, 1.0f);
    EXPECT_EQ(source.boneIds[3][1], 5);

    const std::vector<Matrix4> skin = {Matrix4::translateMatrix(Vector3(0, 2, 0)),
                                       Matrix4::translateMatrix(Vector3(3, 0, 0)) * axisAngle(90.0, 0.0, 0.0, 1.0).getRotationMatrix()};
    CpuSkinResult result;
    ASSERT_TRUE(cpuDeform(source, skin, nullptr, nullptr, result, &error)) << error;
    std::vector<M4> mats = {fromMatrix(skin[0]), fromMatrix(skin[1])};
    for (uint32_t v = 0; v < source.vertexCount(); v++) {
        const uint32_t e = source.vertexIndex[v];
        int jointIds[4];
        double w[4];
        for (int k = 0; k < 4; k++) {
            jointIds[k] = (int)ids[e][k];
            w[k] = weights[e][k];
        }
        // bone 5 is past the matrices: a zero matrix, so the position divides by the other weight
        if (jointIds[1] == 5) w[1] = 0.0;
        const M4 b = blend(mats, jointIds, w);
        EXPECT_LT(distance(result.positions[v], point(b, positions[e])), 1e-5) << "vertex " << v;
        EXPECT_LT(distance(result.normals[v], direction(b, Vector3(0, 0, 2))), 1e-5) << "vertex " << v;
    }

    std::string missing;
    EXPECT_FALSE(cpuDeform(source, {}, nullptr, nullptr, result, &missing));
    EXPECT_NE(missing.find("no skin matrices"), std::string::npos) << missing;
}

// ==================================================================== lattices

TEST_F(CpuSkin_, LatticeStagesMatchDeformVertex) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity column = loadModel(sceneId, "models/column.gltf");
    const Entity meshEntity = meshEntityOf(scene, column);
    ASSERT_EQ(meshEntity, column);
    ModelComponent& model = scene->getComponent<ModelComponent>(column);
    const tinygltf::Model& g = *model.gltfModel;

    CpuSkinSource source;
    std::string error;
    ASSERT_TRUE(buildSource(scene, meshEntity, source, &error)) << error;
    Vector3 center, size;
    bounds(source.positions, center, size);
    // stage 0 around the bind pose, stage 1 around (roughly) the posed one
    const Entity before = addLattice(scene, center, size, 3, 3, 3, LatticeInterpolation::CUBIC, LatticeOutside::FADE, 0.0f, 0.12f);
    const Entity after = addLattice(scene, center + Vector3(0.05f, 0.0f, 0.0f), size * 1.3f, 2, 3, 2, LatticeInterpolation::SMOOTH,
                                    LatticeOutside::EXTEND, 7.0f, 0.1f);
    bindLattices(scene, meshEntity, {binding(before, LatticeStage::BEFORE_SKINNING, 1.0f), binding(after, LatticeStage::AFTER_SKINNING, 0.8f)});
    Transform& upper = scene->getComponent<Transform>(jointEntity(model, g.skins[0].joints[1]));
    upper.rotation = upper.rotation * axisAngle(40.0, 1.0, 0.0, 0.0);
    MeshComponent& mesh = scene->getComponent<MeshComponent>(meshEntity);
    mesh.morphWeights[0] = 0.4f;
    updateTransforms(scene);
    LatticeRender::prepare(scene);
    ASSERT_EQ(mesh.lattice.count, 2);

    // the packed records decode to the components' values
    CpuSkinLattice inputs;
    ASSERT_TRUE(latticeInputs(mesh, inputs, &error)) << error;
    ASSERT_EQ(inputs.bindings.size(), 2u);
    const std::unique_ptr<DirectLattice> directBefore = directLattice(scene, before, meshEntity, 1.0f);
    const std::unique_ptr<DirectLattice> directAfter = directLattice(scene, after, meshEntity, 0.8f);
    EXPECT_EQ(inputs.bindings[0].stage, 0);
    EXPECT_EQ(inputs.bindings[1].stage, 1);
    EXPECT_EQ(inputs.bindings[1].weight, 0.8f);
    EXPECT_EQ(inputs.bindings[0].padded, directBefore->padded);
    EXPECT_EQ(inputs.bindings[1].padded, directAfter->padded);
    EXPECT_EQ(inputs.bindings[0].objectToNormalized, directBefore->binding.objectToNormalized);
    EXPECT_EQ(inputs.bindings[1].lattice.interpolation, LatticeInterpolation::SMOOTH);
    EXPECT_EQ(inputs.bindings[1].lattice.outside, LatticeOutside::EXTEND);
    EXPECT_EQ(inputs.bindings[1].lattice.resolutionX, 2);
    Vector4 block[2];
    LatticeRender::getUniformBlock(mesh.lattice, 0, false, block);
    EXPECT_EQ((uint32_t)block[0].y, (inputs.bindings[0].moving ? 1u : 0u) | (inputs.bindings[1].moving ? 2u : 0u));
    EXPECT_TRUE(inputs.bindings[0].moving && inputs.bindings[1].moving);

    CpuSkinResult result, plain;
    ASSERT_TRUE(deformEntity(scene, meshEntity, result, &error)) << error;
    std::vector<Matrix4> skin;
    ASSERT_TRUE(computeSkinMatrices(scene, meshEntity, skin, &error)) << error;
    ASSERT_TRUE(cpuDeform(source, skin, mesh.morphWeights, nullptr, plain, &error)) << error;

    // the reference: morph, lattice::deformVertex (stage 0), plain-math skin, deformVertex (stage 1)
    const tinygltf::Primitive& prim = g.meshes[0].primitives[0];
    const std::vector<double> positions = readAccessor(g, prim.attributes.at("POSITION"));
    const std::vector<double> normals = readAccessor(g, prim.attributes.at("NORMAL"));
    const std::vector<double> joints = readAccessor(g, prim.attributes.at("JOINTS_0"));
    const std::vector<double> weights = readAccessor(g, prim.attributes.at("WEIGHTS_0"));
    const std::vector<double> morph = readAccessor(g, prim.targets[0].at("POSITION"));
    const std::vector<double> ibm = readAccessor(g, g.skins[0].inverseBindMatrices);
    const M4 inverseMesh = inverse(worldOf(scene, meshEntity));
    std::vector<M4> mats;
    for (size_t j = 0; j < g.skins[0].joints.size(); j++) {
        float columns[16];
        for (int i = 0; i < 16; i++) columns[i] = (float)ibm[j * 16 + static_cast<size_t>(i)];
        mats.push_back(mul(mul(inverseMesh, worldOf(scene, jointEntity(model, g.skins[0].joints[j]))), fromColumns(columns)));
    }
    MaxError position, normal;
    double latticeMoved = 0.0;
    for (uint32_t v = 0; v < source.vertexCount(); v++) {
        const size_t e = source.vertexIndex[v];
        Vector3 p((float)(positions[e * 3] + 0.4 * morph[e * 3]), (float)(positions[e * 3 + 1] + 0.4 * morph[e * 3 + 1]),
                  (float)(positions[e * 3 + 2] + 0.4 * morph[e * 3 + 2]));
        Vector3 n((float)normals[e * 3], (float)normals[e * 3 + 1], (float)normals[e * 3 + 2]);
        Vector3 t(1.0f, 0.0f, 0.0f);
        lattice::deformVertex(directBefore->binding, p, &n, &t, nullptr, nullptr);
        int ids[4];
        double w[4];
        for (int k = 0; k < 4; k++) {
            ids[k] = (int)joints[e * 4 + static_cast<size_t>(k)];
            w[k] = weights[e * 4 + static_cast<size_t>(k)];
        }
        const M4 b = blend(mats, ids, w);
        p = point(b, p);
        n = direction(b, n);
        t = direction(b, t);
        lattice::deformVertex(directAfter->binding, p, &n, &t, nullptr, nullptr);
        position.add(distance(result.positions[v], p), v);
        normal.add(distance(result.normals[v], n), v);
        latticeMoved = std::max(latticeMoved, distance(result.positions[v], plain.positions[v]));
    }
    EXPECT_LT(position.value, 1e-4) << "vertex " << position.at;
    EXPECT_LT(normal.value, 1e-4) << "vertex " << normal.at;
    EXPECT_GT(latticeMoved, 0.01) << "the lattices deform the column";

    // lattices at rest leave the draw (not moving): exactly the mesh without them
    scene->getComponent<LatticeComponent>(before).offsets.assign(27, Vector3(0, 0, 0));
    scene->getComponent<LatticeComponent>(after).offsets.assign(12, Vector3(0, 0, 0));
    LatticeRender::prepare(scene);
    ASSERT_TRUE(latticeInputs(mesh, inputs, &error)) << error;
    ASSERT_EQ(inputs.bindings.size(), 2u);
    EXPECT_FALSE(inputs.bindings[0].moving || inputs.bindings[1].moving);
    ASSERT_TRUE(deformEntity(scene, meshEntity, result, &error)) << error;
    for (uint32_t v = 0; v < source.vertexCount(); v++) {
        EXPECT_EQ(result.positions[v], plain.positions[v]);
        EXPECT_EQ(result.normals[v], plain.normals[v]);
    }
}

// A mesh without skinning takes every binding after skinning, even one marked before
TEST_F(CpuSkin_, LatticeOnARigidMeshIsAlwaysStageOne) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity box = scene->createUserEntity();
    scene->addComponent<Transform>(box, {});
    scene->addComponent<MeshComponent>(box, {});
    scene->getSystem<MeshSystem>()->createBox(scene->getComponent<MeshComponent>(box), 1.0f, 1.0f, 1.0f);
    Transform& placement = scene->getComponent<Transform>(box);
    placement.position = Vector3(0.3f, 0.0f, -0.2f);
    placement.rotation = axisAngle(25.0, 0.0, 1.0, 0.0);
    const Entity cage = addLattice(scene, Vector3(0.3f, 0.0f, -0.2f), Vector3(1.6f, 1.6f, 1.6f), 3, 3, 3,
                                   LatticeInterpolation::CUBIC, LatticeOutside::FADE, 3.0f, 0.15f);
    bindLattices(scene, box, {binding(cage, LatticeStage::BEFORE_SKINNING, 1.0f)});
    updateTransforms(scene);
    LatticeRender::prepare(scene);

    CpuSkinSource source;
    CpuSkinResult result;
    std::string error;
    ASSERT_TRUE(deformEntity(scene, box, result, &error, &source)) << error;
    const std::unique_ptr<DirectLattice> direct = directLattice(scene, cage, box, 1.0f);
    MaxError position, normal;
    double moved = 0.0;
    for (uint32_t v = 0; v < source.vertexCount(); v++) {
        Vector3 p = source.positions[v];
        Vector3 n = unit(source.normals[v]);
        Vector3 t(1.0f, 0.0f, 0.0f);
        lattice::deformVertex(direct->binding, p, &n, &t, nullptr, nullptr);
        position.add(distance(result.positions[v], p), v);
        normal.add(distance(result.normals[v], n), v);
        moved = std::max(moved, distance(result.positions[v], source.positions[v]));
    }
    EXPECT_LT(position.value, 1e-5) << "vertex " << position.at;
    EXPECT_LT(normal.value, 1e-5) << "vertex " << normal.at;
    EXPECT_GT(moved, 0.01);
}

// A binding limited to one submesh with a red mask: the other submesh's copies of the shared
// vertices stay where skinning puts them
TEST_F(CpuSkin_, LatticeMaskAndSubmeshBinding) {
    const Layout d = layoutData(MorphMode::PositionNormalTangent);
    writeLayout("models/gen/masked.gltf", d);
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity model = loadModel(sceneId, "models/gen/masked.gltf");
    ModelComponent& component = scene->getComponent<ModelComponent>(model);
    Transform& mid = scene->getComponent<Transform>(jointEntity(component, 2));
    mid.rotation = mid.rotation * axisAngle(30.0, 0.0, 0.0, 1.0);
    updateTransforms(scene);

    CpuSkinSource source;
    CpuSkinResult plain, result;
    std::string error;
    ASSERT_TRUE(deformEntity(scene, model, plain, &error, &source)) << error;
    Vector3 center, size;
    bounds(plain.positions, center, size);
    const Entity cage = addLattice(scene, center, size, 3, 3, 3, LatticeInterpolation::SHARP, LatticeOutside::EXTEND, 11.0f, 0.2f);
    const unsigned int maskedSubmesh = source.submeshes[1].submesh;
    bindLattices(scene, model, {binding(cage, LatticeStage::AFTER_SKINNING, 1.0f, (int)maskedSubmesh, LatticeMaskChannel::RED)});
    updateTransforms(scene);
    LatticeRender::prepare(scene);
    ASSERT_TRUE(deformEntity(scene, model, result, &error)) << error;

    const std::unique_ptr<DirectLattice> direct = directLattice(scene, cage, model, 1.0f, LatticeMaskChannel::RED);
    MaxError position, normal, tangent;
    int differing = 0;
    for (uint32_t v = 0; v < source.vertexCount(); v++) {
        Vector3 p = plain.positions[v];
        Vector3 n = plain.normals[v];
        Vector3 t(plain.tangents[v].x, plain.tangents[v].y, plain.tangents[v].z);
        if (source.vertexSubmesh[v] == maskedSubmesh) {
            const Vector4 color = source.colors[v];
            lattice::deformVertex(direct->binding, p, &n, &t, nullptr, &color);
        }
        position.add(distance(result.positions[v], p), v);
        normal.add(distance(result.normals[v], n), v);
        tangent.add(distance(Vector3(result.tangents[v].x, result.tangents[v].y, result.tangents[v].z), t), v);
        EXPECT_EQ(result.tangents[v].w, plain.tangents[v].w);
        if (source.canonical[v] != v && distance(result.positions[v], result.positions[source.canonical[v]]) > 1e-3) differing++;
    }
    EXPECT_LT(position.value, 1e-5) << "vertex " << position.at;
    EXPECT_LT(normal.value, 1e-5) << "vertex " << normal.at;
    EXPECT_LT(tangent.value, 1e-5) << "vertex " << tangent.at;
    EXPECT_GT(differing, 0) << "shared vertices part where one submesh's lattice moves them (red is not 0 there)";
}

// ==================================================================== unsupported layouts

TEST_F(CpuSkin_, UnsupportedLayoutsFailWithTheReason) {
    std::string error;
    CpuSkinSource source;

    // a normalized byte VEC3 normal of a file (mesh quantization) loads as the BYTE4N the renderer
    // binds (S53 R1), so it skins with its normals; a layout no vertex format binds is refused below
    writeLayout("models/gen/bad_normal.gltf", layoutData(MorphMode::PositionOnly), true);
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    const Entity quantized = loadModel(sceneId, "models/gen/bad_normal.gltf");
    ASSERT_TRUE(buildSource(scene, quantized, source, &error)) << error;
    ASSERT_FALSE(source.normals.empty());
    EXPECT_NEAR(source.normals[0].z, 1.0f, 1e-6f);

    // a model root whose geometry is on its mesh node
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    EXPECT_FALSE(buildSource(scene, hero, source, &error));
    EXPECT_NE(error.find("mesh node entities"), std::string::npos) << error;

    // no submeshes
    MeshComponent empty;
    EXPECT_FALSE(buildSource(empty, source, &error));
    EXPECT_NE(error.find("no submeshes"), std::string::npos) << error;

    // three vertices in an external buffer, as glTF meshes bind them
    const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    auto external = [&positions](MeshComponent& mesh, unsigned int stride) {
        mesh.eBuffers[0].setData(reinterpret_cast<unsigned char*>(const_cast<float*>(positions)), sizeof(positions));
        mesh.eBuffers[0].setStride(stride);
        mesh.eBuffers[0].setName("positions");
        mesh.eBuffers[0].setRenderAttributes(false);
        mesh.numExternalBuffers = 1;
        mesh.numSubmeshes = 1;
        mesh.vertexCount = 3;
    };
    {
        MeshComponent mesh;
        external(mesh, 12);
        mesh.submeshes[0].attributes[AttributeType::POSITION] = Attribute(AttributeDataType::FLOAT, "positions", 3, 0, 3, false, false);
        ASSERT_TRUE(buildSource(mesh, source, &error)) << "the valid form: " << error;
        EXPECT_EQ(source.vertexCount(), 3u) << "no indices: the mesh's vertex count";
        EXPECT_EQ(source.positions[1], Vector3(1, 0, 0));

        mesh.submeshes[0].attributes[AttributeType::NORMAL] = Attribute(AttributeDataType::FLOAT, "nowhere", 3, 0, 3, false, false);
        EXPECT_FALSE(buildSource(mesh, source, &error));
        EXPECT_NE(error.find("NORMAL reads the buffer 'nowhere'"), std::string::npos) << error;
        mesh.submeshes[0].attributes.erase(AttributeType::NORMAL);

        // an index past the data
        const uint16_t indices[4] = {0, 1, 5, 0};
        ExternalBuffer& indexBuffer = mesh.eBuffers[1];
        indexBuffer.setData(reinterpret_cast<unsigned char*>(const_cast<uint16_t*>(indices)), sizeof(indices));
        indexBuffer.setStride(2);
        indexBuffer.setName("indices16");
        indexBuffer.setRenderAttributes(false);
        mesh.numExternalBuffers = 2;
        mesh.submeshes[0].attributes[AttributeType::INDEX] = Attribute(AttributeDataType::UNSIGNED_SHORT, "indices16", 1, 0, 3, false, false);
        EXPECT_FALSE(buildSource(mesh, source, &error));
        EXPECT_NE(error.find("vertex 5 of POSITION lies past the buffer 'positions'"), std::string::npos) << error;

        // more indices than the buffer holds
        mesh.submeshes[0].attributes[AttributeType::INDEX] = Attribute(AttributeDataType::UNSIGNED_SHORT, "indices16", 1, 0, 9, false, false);
        EXPECT_FALSE(buildSource(mesh, source, &error));
        EXPECT_NE(error.find("run past the index buffer"), std::string::npos) << error;

        // 8-bit indices are not drawn
        mesh.submeshes[0].attributes[AttributeType::INDEX] = Attribute(AttributeDataType::UNSIGNED_BYTE, "indices16", 1, 0, 3, false, false);
        EXPECT_FALSE(buildSource(mesh, source, &error));
        EXPECT_NE(error.find("index type UNSIGNED_BYTE"), std::string::npos) << error;
    }
    {
        MeshComponent mesh;
        external(mesh, 0);
        mesh.submeshes[0].attributes[AttributeType::POSITION] = Attribute(AttributeDataType::FLOAT, "positions", 3, 0, 3, false, false);
        EXPECT_FALSE(buildSource(mesh, source, &error));
        EXPECT_NE(error.find("has no stride"), std::string::npos) << error;
    }
    {
        MeshComponent mesh;
        external(mesh, 12);
        mesh.submeshes[0].attributes[AttributeType::NORMAL] = Attribute(AttributeDataType::FLOAT, "positions", 3, 0, 3, false, false);
        EXPECT_FALSE(buildSource(mesh, source, &error));
        EXPECT_NE(error.find("no POSITION"), std::string::npos) << error;
    }
    {
        MeshComponent mesh;
        external(mesh, 12);
        mesh.submeshes[0].attributes[AttributeType::POSITION] = Attribute(AttributeDataType::FLOAT, "positions", 3, 0, 3, false, false);
        mesh.submeshes[0].attributes[AttributeType::TEXCOORD1] = Attribute(AttributeDataType::UNSIGNED_BYTE, "positions", 2, 0, 3, true, false);
        EXPECT_FALSE(buildSource(mesh, source, &error));
        EXPECT_NE(error.find("TEXCOORD1 is 2 x UNSIGNED_BYTE (normalized)"), std::string::npos) << error;
    }
    {
        MeshComponent mesh;
        external(mesh, 12);
        mesh.submeshes[0].attributes[AttributeType::POSITION] = Attribute(AttributeDataType::FLOAT, "positions", 3, 0, 3, false, false);
        mesh.submeshes[0].attributes[AttributeType::NORMAL] = Attribute(AttributeDataType::BYTE, "positions", 3, 0, 3, true, false);
        EXPECT_FALSE(buildSource(mesh, source, &error));
        EXPECT_NE(error.find("NORMAL is 3 x BYTE (normalized)"), std::string::npos) << error;
    }

    // a lattice record cut short
    MeshComponent latticeMesh;
    latticeMesh.lattice.count = 1;
    latticeMesh.lattice.texels.assign(8, Vector4(0, 0, 0, 0));
    CpuSkinLattice inputs;
    EXPECT_FALSE(latticeInputs(latticeMesh, inputs, &error));
    EXPECT_NE(error.find("runs past the packed texels"), std::string::npos) << error;
}
