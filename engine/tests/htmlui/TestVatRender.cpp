// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Vertex Animation (VAT) GPU path (docs/superpowers/specs/2026-10-27-vertex-animation-design.md
// 2.2, 2.3, 3.2.3-3.2.5, 3.5, 3.6) drawn by the engine into the harness target: .vat.glb-like
// meshes written as glTF in a temporary directory (_VAT_INDEX, JOINTS_0 / WEIGHTS_0 without a
// skin, the JASPER_vertex_animation extension), in-memory assets made with
// vat::VertexAnimation::create and put in the VertexAnimationPool, and a Vertex Animator played
// by its system (paused, so its clock stays at 0: a track pair at time t is set with its times
// moved by -t), which writes the GPU state the renderer reads. An orthographic camera looks down
// -z at 50 pixels per world unit. Vertices mode moves vertices by the baked offsets (frames,
// interpolation, loop seam, once clamp, ping-pong, stepped, absolute positions), crossfades and
// phase-synced blends, turns normals by the tangent frame; Bones mode skins rigid pieces (a fast
// spin keeps its size); instances play their own state after sorting; shadows and the depth
// pre-pass follow; animatedBounds keeps a moved mesh from being culled; a KHR_materials_unlit
// material (what the baker writes for an unlit source) draws its base colour whatever the
// lights, played or not; and the shader's frame math matches vat::vatSample / vat::vatWeightB
// over a grid of times.

#include "TestGoldenCommon.h"

#include "Scene.h"
#include "animation/VertexAnimation.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "pool/VertexAnimationPool.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VertexAnimatorSystem.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int screenWidth = 640;
    const int screenHeight = 360;
    const float pixelsPerUnit = 50.0f;

    float toScreenX(float x){ return screenWidth * 0.5f + x * pixelsPerUnit; }
    float toScreenY(float y){ return screenHeight * 0.5f - y * pixelsPerUnit; }
    int toScreenRow(float y){ return int(std::lround(screenHeight * 0.5f - y * pixelsPerUnit)); }

    // ---- glTF meshes as the baker writes them (one node, identity transform, no skin)

    struct VatGltf {
        std::vector<float> positions;   // xyz
        std::vector<float> normals;     // xyz
        std::vector<float> tangents;    // xyzw
        std::vector<float> vatIndex;    // _VAT_INDEX
        std::vector<uint16_t> joints;   // JOINTS_0 (Bones), 4 per vertex
        bool shortJoints = false;       // written as UNSIGNED_SHORT (else UNSIGNED_BYTE)
        std::vector<float> weights;     // WEIGHTS_0 (Bones), 4 per vertex
        std::vector<uint16_t> indices;
        float color[3] = {0.9f, 0.4f, 0.2f};
        bool extension = true;          // JASPER_vertex_animation
        bool unlit = false;             // the material has KHR_materials_unlit

        int vertexCount() const{ return int(positions.size() / 3); }

        // a box of 24 vertices (4 per face, outward normals, tangents with w = 1), every vertex
        // its own _VAT_INDEX, all on `bone` (weight 1) when joints are written
        void addBox(const Vector3& center, const Vector3& half, int bone = -1){
            struct Face { Vector3 n; Vector3 t; };
            const Face faces[6] = {
                {Vector3(0, 0, 1), Vector3(1, 0, 0)}, {Vector3(0, 0, -1), Vector3(-1, 0, 0)},
                {Vector3(1, 0, 0), Vector3(0, 0, -1)}, {Vector3(-1, 0, 0), Vector3(0, 0, 1)},
                {Vector3(0, 1, 0), Vector3(1, 0, 0)}, {Vector3(0, -1, 0), Vector3(1, 0, 0)}};
            for (const Face& face : faces){
                const Vector3 b = face.n.crossProduct(face.t);
                const uint16_t first = uint16_t(vertexCount());
                const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                for (const auto& c : corners){
                    const Vector3 local = face.n + face.t * c[0] + b * c[1];
                    const Vector3 p(center.x + local.x * half.x, center.y + local.y * half.y, center.z + local.z * half.z);
                    positions.insert(positions.end(), {p.x, p.y, p.z});
                    normals.insert(normals.end(), {face.n.x, face.n.y, face.n.z});
                    tangents.insert(tangents.end(), {face.t.x, face.t.y, face.t.z, 1.0f});
                    vatIndex.push_back(float(vatIndex.size()));
                    if (bone >= 0){
                        joints.insert(joints.end(), {uint16_t(bone), 0, 0, 0});
                        weights.insert(weights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
                    }
                }
                indices.insert(indices.end(), {first, uint16_t(first + 1), uint16_t(first + 2), first, uint16_t(first + 2), uint16_t(first + 3)});
            }
        }

        Vector3 position(int v) const{ return Vector3(positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]); }
    };

    std::string base64(const std::vector<uint8_t>& bytes){
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3){
            const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += table[v & 63];
        }
        if (i + 1 == bytes.size()){
            const uint32_t v = uint32_t(bytes[i]) << 16;
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += "==";
        }else if (i + 2 == bytes.size()){
            const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += '=';
        }
        return out;
    }

    std::filesystem::path testDirectory(){
        static const std::filesystem::path dir = [](){
            std::filesystem::path path = std::filesystem::temp_directory_path() / ("jasper-vat-render-" + std::to_string(::getpid()));
            std::filesystem::create_directories(path);
            return path;
        }();
        return dir;
    }

    // writes the mesh as <name>/mesh.gltf in the test directory (the mesh the assets name),
    // returns its absolute path
    std::string writeGltf(const VatGltf& mesh, const std::string& name){
        std::vector<uint8_t> data;
        std::ostringstream views;
        std::ostringstream accessors;
        int count = 0;
        auto append = [&](const void* bytes, size_t size, int target){
            while (data.size() % 4)
                data.push_back(0);
            const size_t offset = data.size();
            data.insert(data.end(), (const uint8_t*)bytes, (const uint8_t*)bytes + size);
            views << (count ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << size << ",\"target\":" << target << "}";
            return count++;
        };
        auto accessor = [&](int view, int componentType, int elements, const char* type, const std::string& extra = ""){
            accessors << (view ? "," : "") << "{\"bufferView\":" << view << ",\"componentType\":" << componentType << ",\"count\":" << elements
                      << ",\"type\":\"" << type << "\"" << extra << "}";
        };
        const int vertices = mesh.vertexCount();
        Vector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
        for (int v = 0; v < vertices; v++){
            const Vector3 p = mesh.position(v);
            lo = Vector3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vector3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        std::ostringstream minmax;
        minmax << ",\"min\":[" << lo.x << "," << lo.y << "," << lo.z << "],\"max\":[" << hi.x << "," << hi.y << "," << hi.z << "]";

        std::ostringstream attributes;
        accessor(append(mesh.positions.data(), mesh.positions.size() * 4, 34962), 5126, vertices, "VEC3", minmax.str());
        attributes << "\"POSITION\":0";
        accessor(append(mesh.normals.data(), mesh.normals.size() * 4, 34962), 5126, vertices, "VEC3");
        attributes << ",\"NORMAL\":1";
        accessor(append(mesh.tangents.data(), mesh.tangents.size() * 4, 34962), 5126, vertices, "VEC4");
        attributes << ",\"TANGENT\":2";
        int next = 3;
        if (!mesh.vatIndex.empty()){
            accessor(append(mesh.vatIndex.data(), mesh.vatIndex.size() * 4, 34962), 5126, vertices, "SCALAR");
            attributes << ",\"" << vat::VAT_INDEX_ATTRIBUTE << "\":" << next++;
        }
        if (!mesh.joints.empty()){
            if (mesh.shortJoints){
                accessor(append(mesh.joints.data(), mesh.joints.size() * 2, 34962), 5123, vertices, "VEC4");
            }else{
                const std::vector<uint8_t> bytes(mesh.joints.begin(), mesh.joints.end());
                accessor(append(bytes.data(), bytes.size(), 34962), 5121, vertices, "VEC4");
            }
            attributes << ",\"JOINTS_0\":" << next++;
            accessor(append(mesh.weights.data(), mesh.weights.size() * 4, 34962), 5126, vertices, "VEC4");
            attributes << ",\"WEIGHTS_0\":" << next++;
        }
        const int indexAccessor = next;
        accessor(append(mesh.indices.data(), mesh.indices.size() * 2, 34963), 5123, int(mesh.indices.size()), "SCALAR");

        std::ostringstream json;
        json << "{\"asset\":{\"version\":\"2.0\"},";
        if (mesh.extension){
            json << "\"extensionsUsed\":[\"" << vat::VAT_GLTF_EXTENSION << "\"],\"extensions\":{\"" << vat::VAT_GLTF_EXTENSION
                 << "\":{\"version\":1,\"layoutHash\":\"test\"}},";
        }
        json << "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"name\":\"" << name << "\"}],"
             << "\"meshes\":[{\"primitives\":[{\"attributes\":{" << attributes.str() << "},\"indices\":" << indexAccessor << ",\"material\":0}]}],"
             << "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" << mesh.color[0] << "," << mesh.color[1] << "," << mesh.color[2]
             << ",1],\"metallicFactor\":0,\"roughnessFactor\":1}" << (mesh.unlit ? ",\"extensions\":{\"KHR_materials_unlit\":{}}" : "") << "}],"
             << "\"buffers\":[{\"byteLength\":" << data.size() << ",\"uri\":\"data:application/octet-stream;base64," << base64(data) << "\"}],"
             << "\"bufferViews\":[" << views.str() << "],\"accessors\":[" << accessors.str() << "]}";
        std::filesystem::create_directories(testDirectory() / name);
        const std::filesystem::path path = testDirectory() / name / "mesh.gltf";
        std::ofstream(path) << json.str();
        return path.string();
    }

    // ---- in-memory assets

    struct ClipSpec {
        std::string name;
        int frames = 1;
        float fps = 1.0f;
        ClipWrapMode wrap = ClipWrapMode::Loop;
        bool interpolate = true;
        bool restNone = false;
    };

    // texel(globalFrame, element, k): the payload, RGBA32F, 64 texels a row; played on the
    // mesh.gltf next to it; bounds: the asset's (every frame), none by default
    std::shared_ptr<const vat::VertexAnimation> makeAsset(vat::VatMode mode, int elements, bool normals, const std::vector<ClipSpec>& clips,
                                                         const std::function<Vector4(int, int, int)>& texel,
                                                         const vat::VatBounds& bounds = vat::VatBounds()){
        vat::VatData data;
        data.mode = mode;
        data.mesh = "mesh.gltf";
        data.bounds = bounds;
        int start = 0;
        for (const ClipSpec& spec : clips){
            vat::VatClip clip;
            clip.name = spec.name;
            clip.start = start;
            clip.frames = spec.frames;
            clip.fps = spec.fps;
            clip.wrap = spec.wrap;
            clip.interpolate = spec.interpolate;
            clip.restNone = spec.restNone;
            start += spec.frames;
            data.clips.push_back(clip);
        }
        data.layout.precision = vat::VatPrecision::Float;
        data.layout.width = 64;
        data.layout.samples = start;
        data.layout.normals = normals && mode == vat::VatMode::Vertices;
        if (mode == vat::VatMode::Bones)
            data.layout.bones = elements;
        else
            data.layout.vertices = elements;
        const int texelsPerSample = data.layout.texelsPerSample(mode);
        const int texels = start * elements * texelsPerSample;
        data.layout.height = std::max(1, (texels + data.layout.width - 1) / data.layout.width);
        data.layout.layoutHash = vat::computeLayoutHash(data);

        vat::VatPayload payload;
        payload.precision = vat::VatPrecision::Float;
        payload.width = data.layout.width;
        payload.height = data.layout.height;
        payload.bytes.assign(size_t(payload.width) * payload.height * 16, 0);
        for (int frame = 0; frame < start; frame++){
            for (int element = 0; element < elements; element++){
                for (int k = 0; k < texelsPerSample; k++){
                    const uint64_t t = vat::vatTexelIndex(mode, data.layout, frame, element, k);
                    const Vector4 value = texel(frame, element, k);
                    const float rgba[4] = {value.x, value.y, value.z, value.w};
                    std::memcpy(&payload.bytes[size_t(t) * 16], rgba, 16);
                }
            }
        }
        std::string error;
        std::shared_ptr<const vat::VertexAnimation> asset = vat::VertexAnimation::create(data, payload, vat::VatSocketTracks(), &error);
        EXPECT_TRUE(asset) << error;
        return asset;
    }

    vat::VatTrackPair track(int clip, float start = 0.0f, float speed = 1.0f){
        vat::VatTrackPair pair;
        pair.a.clip = clip;
        pair.a.start = start;
        pair.a.speed = speed;
        return pair;
    }

    // unit quaternion (x, y, z, w) of an angle (radians) about an axis
    Vector4 axisAngle(const Vector3& axis, float angle){
        const Vector3 a = axis.normalized();
        const float s = std::sin(angle * 0.5f);
        return Vector4(a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f));
    }

    Vector3 rotate(const Vector4& q, const Vector3& v){
        const Vector3 u(q.x, q.y, q.z);
        return v + u.crossProduct(u.crossProduct(v) + v * q.w) * 2.0f;
    }

    Vector4 multiply(const Vector4& a, const Vector4& b){
        return Vector4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                       a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                       a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
    }

    // the quaternion of an orthonormal frame taking +X, +Y, +Z to t, b, n
    Vector4 frameQuaternion(const Vector3& t, const Vector3& b, const Vector3& n){
        const float m00 = t.x, m01 = b.x, m02 = n.x, m10 = t.y, m11 = b.y, m12 = n.y, m20 = t.z, m21 = b.z, m22 = n.z;
        const float trace = m00 + m11 + m22;
        Vector4 q;
        if (trace > 0.0f){
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q = Vector4((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
        }else if (m00 > m11 && m00 > m22){
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q = Vector4(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
        }else if (m11 > m22){
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q = Vector4((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
        }else{
            const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q = Vector4((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
        }
        return q;
    }

    // ---- the scene

    struct VatScene {
        SceneFixture fixture;
        Camera camera;

        VatScene(): camera(&fixture.getScene()){
            camera.setOrtho(-6.4f, 6.4f, -3.6f, 3.6f, 0.1f, 100.0f);
            camera.setPosition(0.0f, 0.0f, 10.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            // the animators' clock stays at 0 (edit mode without Preview)
            scene().getSystem<VertexAnimatorSystem>()->setPaused(true);
        }

        // a track pair at time `now` of a clock at 0: its times moved by -now (a held track's
        // start is its local time: it stays)
        static vat::VatTrackPair at(vat::VatTrackPair pair, float now){
            if (pair.a.speed != 0.0f)
                pair.a.start -= now;
            if (pair.b.speed != 0.0f)
                pair.b.start -= now;
            pair.fadeStart -= now;
            pair.fadeEnd -= now;
            return pair;
        }

        Scene& scene(){ return fixture.getScene(); }

        Model& addModel(const std::string& path){
            Model& model = fixture.add<Model>();
            EXPECT_TRUE(model.loadModel(path)) << path;
            return model;
        }

        // The entity's Vertex Animator playing `asset` (put in the pool as a .vat next to the
        // Model's mesh.gltf, the mesh it is baked for) with `pair` at time `now`: a new asset is
        // loaded and started by the system first (which plays nothing: playAutomatically off),
        // then the state is set as a script's play would leave it
        VertexAnimatorComponent& animate(Entity entity, const std::shared_ptr<const vat::VertexAnimation>& asset, const vat::VatTrackPair& pair, float now){
            const ModelComponent& model = scene().getComponent<ModelComponent>(entity);
            char name[64];
            std::snprintf(name, sizeof(name), "anim_%p.vat", (const void*)asset.get());
            const std::string path = (std::filesystem::path(model.filename).parent_path() / name).generic_string();
            if (VertexAnimationPool::get(path) != asset)
                VertexAnimationPool::put(path, asset);
            if (!scene().findComponent<VertexAnimatorComponent>(entity)){
                VertexAnimatorComponent added;
                added.playAutomatically = false;
                added.randomStart = false;
                scene().addComponent<VertexAnimatorComponent>(entity, added);
            }
            if (scene().getComponent<VertexAnimatorComponent>(entity).animation != path){
                scene().getComponent<VertexAnimatorComponent>(entity).animation = path;
                Scene* current = &scene();
                EXPECT_TRUE(GpuEnvironment::runFrames([current, entity, &asset](){
                    const VertexAnimatorComponent& animator = current->getComponent<VertexAnimatorComponent>(entity);
                    return animator.started && animator.asset == asset;
                }, 120)) << "the animator loads and starts " << path;
            }
            VertexAnimatorComponent& animator = scene().getComponent<VertexAnimatorComponent>(entity);
            animator.state = at(pair, now);
            return animator;
        }

        // one instance's state (the animator started: animate first)
        void animateInstance(Entity entity, int index, const vat::VatTrackPair& pair, float now){
            VertexAnimatorComponent& animator = scene().getComponent<VertexAnimatorComponent>(entity);
            InstancedMeshComponent& instmesh = scene().getComponent<InstancedMeshComponent>(entity);
            if (animator.instanceStates.size() < instmesh.instances.size())
                animator.instanceStates.resize(instmesh.instances.size());
            animator.instanceStates[size_t(index)] = at(pair, now);
            // the system writes InstanceData::vat0/vat1 again from the states
            instmesh.needUpdateInstances = true;
        }

        // frames until everything is loaded and every mesh draws the variant its animator asks
        // for (a change reloads the mesh once the new shaders are built), then two more
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
                return ++extra > 2;
            }, 600);
        }

        // a state change draws without a reload
        void draw(){
            int frames = 0;
            GpuEnvironment::runFrames([&frames](){ return ++frames > 2; }, 10);
        }
    };

    bool sameImage(const Screen& a, const Screen& b, int& differing){
        differing = 0;
        if (a.width != b.width || a.height != b.height)
            return false;
        for (size_t p = 0; p + 3 < a.rgba.size(); p += 4){
            if (a.rgba[p] != b.rgba[p] || a.rgba[p + 1] != b.rgba[p + 1] || a.rgba[p + 2] != b.rgba[p + 2])
                differing++;
        }
        return differing == 0;
    }

    InkBox rowInk(const Screen& screen, float worldY, int x0 = 0, int x1 = screenWidth){
        const int row = toScreenRow(worldY);
        return screen.inkOnBackground({x0, row, x1, row + 1}, 24);
    }

    InkBox columnInk(const Screen& screen, float worldX, int y0 = 0, int y1 = screenHeight){
        const int column = int(std::lround(toScreenX(worldX)));
        return screen.inkOnBackground({column, y0, column + 1, y1}, 24);
    }

    // the unit box with offsets: the Vertices-mode test asset (no tangent frame). Global frame
    // g of the clips below moves every vertex by offset(g)
    const std::vector<ClipSpec> moveClips = {
        {"Loop", 4, 1.0f, ClipWrapMode::Loop, true},            // frames 0-3: x 0, 0.5, 1, 1.5
        {"Once", 4, 1.0f, ClipWrapMode::Once, true},            // 4-7: the same
        {"Up", 1, 1.0f, ClipWrapMode::Loop, true},              // 8: y 1
        {"PingPong", 4, 1.0f, ClipWrapMode::PingPong, true},    // 9-12: x 0, 0.5, 1, 1.5
        {"Stepped", 4, 1.0f, ClipWrapMode::Loop, false},        // 13-16: x 0, 0.5, 1, 1.5
        {"Absolute", 1, 1.0f, ClipWrapMode::Loop, true, true},  // 17: the rest position + (-1.5, 0, 0), absolute
    };

    Vector3 moveOffset(int frame){
        if (frame < 8)
            return Vector3(0.5f * float(frame % 4), 0.0f, 0.0f);
        if (frame == 8)
            return Vector3(0.0f, 1.0f, 0.0f);
        if (frame < 17)
            return Vector3(0.5f * float((frame - 9) % 4), 0.0f, 0.0f);
        return Vector3(-1.5f, 0.0f, 0.0f);
    }

    std::shared_ptr<const vat::VertexAnimation> makeMoveAsset(const VatGltf& mesh){
        return makeAsset(vat::VatMode::Vertices, mesh.vertexCount(), false, moveClips, [&mesh](int frame, int vertex, int){
            Vector3 offset = moveOffset(frame);
            if (frame == 17)
                offset = offset + mesh.position(vertex);
            return Vector4(offset.x, offset.y, offset.z, 0.0f);
        });
    }

    class VatRenderTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::setView(screenWidth, screenHeight, screenWidth, screenHeight, Scaling::FITWIDTH);
        }
    };

}

TEST_F(VatRenderTest, MeshLoadsTheVatAttributesAndDrawsAtRestWithoutAnimator){
    VatGltf vertices;
    vertices.addBox(Vector3(-2.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
    VatGltf bones;
    bones.addBox(Vector3(2.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f), 1);
    bones.vatIndex.clear();
    VatScene s;
    Model& a = s.addModel(writeGltf(vertices, "load_vertices"));
    Model& b = s.addModel(writeGltf(bones, "load_bones"));
    ASSERT_TRUE(s.settle());

    const MeshComponent& meshA = a.getComponent<MeshComponent>();
    const MeshComponent& meshB = b.getComponent<MeshComponent>();
    EXPECT_TRUE(meshA.vatMesh);
    EXPECT_TRUE(meshB.vatMesh);
    ASSERT_EQ(meshA.numSubmeshes, 1u);
    EXPECT_EQ(meshA.submeshes[0].attributes.count(AttributeType::VAT_INDEX), 1u) << "_VAT_INDEX read as a_vatIndex";
    EXPECT_EQ(meshB.submeshes[0].attributes.count(AttributeType::BONEIDS), 1u) << "JOINTS_0 without a skin";
    EXPECT_EQ(meshB.submeshes[0].attributes.count(AttributeType::BONEWEIGHTS), 1u);
    // without a skin the bones mesh is not a skinned one: it loads and stands at rest
    EXPECT_FALSE(meshB.submeshes[0].hasSkinning);
    EXPECT_EQ(meshB.submeshes[0].shaderProperties & (shaderPropertyBit(13) | ShaderPool::getVatProperties(ShaderType::MESH)), 0u);
    EXPECT_EQ(meshA.submeshes[0].shaderProperties & ShaderPool::getVatProperties(ShaderType::MESH), 0u);

    Screen screen;
    ASSERT_TRUE(screen.read());
    EXPECT_NEAR(rowInk(screen, 0.0f, 0, screenWidth / 2).x0, toScreenX(-2.5f), 1.5f);
    EXPECT_NEAR(rowInk(screen, 0.0f, screenWidth / 2, screenWidth).x0, toScreenX(1.5f), 1.5f);
}

TEST_F(VatRenderTest, VerticesModeMovesByTheBakedOffsets){
    VatGltf box;
    box.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
    const auto asset = makeMoveAsset(box);
    ASSERT_TRUE(asset);
    VatScene s;
    Model& model = s.addModel(writeGltf(box, "move"));
    ASSERT_TRUE(s.settle());
    const uint64_t plain = model.getComponent<MeshComponent>().submeshes[0].shaderProperties;

    s.animate(model.getEntity(), asset, track(0), 0.0f);
    ASSERT_TRUE(s.settle());
    const MeshComponent& mesh = model.getComponent<MeshComponent>();
    EXPECT_EQ(mesh.vat.variantActive, VatVariant::VERTICES);
    EXPECT_EQ(mesh.submeshes[0].shaderProperties, plain | ShaderPool::getVatProperty(ShaderType::MESH, false)) << "the same variant plus 'Vat'";

    struct Case { const char* what; int clip; float now; float speed; float x; float y; };
    const Case cases[] = {
        {"loop, frame 0", 0, 0.0f, 1.0f, 0.0f, 0.0f},
        {"loop, frame 1", 0, 1.0f, 1.0f, 0.5f, 0.0f},
        {"loop, between frames 1 and 2", 0, 1.5f, 1.0f, 0.75f, 0.0f},
        {"loop seam: frame 3 to frame 0", 0, 3.5f, 1.0f, 0.75f, 0.0f},
        {"loop, wrapped", 0, 4.25f, 1.0f, 0.125f, 0.0f},
        {"loop, backwards", 0, 0.5f, -1.0f, 0.75f, 0.0f},
        {"once, past the end", 1, 10.0f, 1.0f, 1.5f, 0.0f},
        {"once, before the start", 1, -2.0f, 1.0f, 0.0f, 0.0f},
        {"ping-pong, coming back", 3, 4.0f, 1.0f, 1.0f, 0.0f},
        {"ping-pong, going", 3, 2.5f, 1.0f, 1.25f, 0.0f},
        {"stepped (no interpolation)", 4, 1.75f, 1.0f, 0.5f, 0.0f},
        {"one frame", 2, 3.0f, 1.0f, 0.0f, 1.0f},
        {"absolute positions (rest none)", 5, 0.0f, 1.0f, -1.5f, 0.0f},
    };
    for (const Case& c : cases){
        SCOPED_TRACE(c.what);
        s.animate(model.getEntity(), asset, track(c.clip, 0.0f, c.speed), c.now);
        s.draw();
        Screen screen;
        ASSERT_TRUE(screen.read());
        EXPECT_NEAR(rowInk(screen, c.y).x0, toScreenX(-0.5f + c.x), 1.5f);
        EXPECT_NEAR(rowInk(screen, c.y).x1 + 1, toScreenX(0.5f + c.x), 1.5f);
        EXPECT_NEAR(columnInk(screen, c.x).y0, toScreenY(0.5f + c.y), 1.5f);
    }
    EXPECT_EQ(mesh.vat.variantActive, VatVariant::VERTICES) << "state changes draw without a reload";
}

TEST_F(VatRenderTest, RestPoseWhenClipIsMinusOneAndAfterTheAnimatorGoes){
    VatGltf box;
    box.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
    const auto asset = makeMoveAsset(box);
    ASSERT_TRUE(asset);
    VatScene s;
    Model& model = s.addModel(writeGltf(box, "rest"));
    ASSERT_TRUE(s.settle());
    const uint64_t plain = model.getComponent<MeshComponent>().submeshes[0].shaderProperties;
    Screen unanimated;
    ASSERT_TRUE(unanimated.read());

    s.animate(model.getEntity(), asset, track(-1), 5.0f);
    ASSERT_TRUE(s.settle());
    Screen rest;
    ASSERT_TRUE(rest.read());
    int differing = 0;
    EXPECT_TRUE(sameImage(unanimated, rest, differing)) << differing << " pixels differ at rest";

    // the animator stops drawing on the GPU: the mesh goes back to the variant without VAT
    s.animate(model.getEntity(), asset, track(0), 1.0f);
    s.draw();
    s.scene().getComponent<VertexAnimatorComponent>(model.getEntity()).enabled = false;
    ASSERT_TRUE(s.settle());
    const MeshComponent& mesh = model.getComponent<MeshComponent>();
    EXPECT_EQ(mesh.vat.variantActive, VatVariant::NONE);
    EXPECT_EQ(mesh.submeshes[0].shaderProperties, plain);
    Screen after;
    ASSERT_TRUE(after.read());
    EXPECT_TRUE(sameImage(unanimated, after, differing)) << differing << " pixels differ without the animator";
}

TEST_F(VatRenderTest, CrossfadeAndPhaseSyncedBlend){
    VatGltf box;
    box.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
    const auto asset = makeMoveAsset(box);
    ASSERT_TRUE(asset);
    VatScene s;
    Model& model = s.addModel(writeGltf(box, "fade"));
    s.animate(model.getEntity(), asset, track(0), 0.0f);
    ASSERT_TRUE(s.settle());

    {
        SCOPED_TRACE("crossfade Loop -> Up from 0 to 2 s, at 1 s");
        vat::VatTrackPair pair = track(0);
        pair.b.clip = 2;
        pair.b.start = 0.0f;
        pair.fadeStart = 0.0f;
        pair.fadeEnd = 2.0f;
        s.animate(model.getEntity(), asset, pair, 1.0f);
        s.draw();
        Screen screen;
        ASSERT_TRUE(screen.read());
        // A at frame 1 (x 0.5), B (y 1), half each
        EXPECT_NEAR(rowInk(screen, 0.5f).x0, toScreenX(-0.5f + 0.25f), 1.5f);
        EXPECT_NEAR(columnInk(screen, 0.25f).y0, toScreenY(0.5f + 0.5f), 1.5f);
    }
    {
        SCOPED_TRACE("crossfade over: B alone");
        vat::VatTrackPair pair = track(0);
        pair.b.clip = 2;
        pair.fadeStart = 0.0f;
        pair.fadeEnd = 2.0f;
        s.animate(model.getEntity(), asset, pair, 3.0f);
        s.draw();
        Screen screen;
        ASSERT_TRUE(screen.read());
        EXPECT_NEAR(rowInk(screen, 1.0f).x0, toScreenX(-0.5f), 1.5f);
        EXPECT_NEAR(columnInk(screen, 0.0f).y0, toScreenY(1.5f), 1.5f);
    }
    {
        SCOPED_TRACE("blend Loop / Once 0.5, B phase-synced");
        vat::VatTrackPair pair = track(0);
        pair.b.clip = 1;
        pair.b.start = 100.0f;      // ignored: B follows A's phase
        pair.blend = 0.5f;
        s.animate(model.getEntity(), asset, pair, 1.0f);
        s.draw();
        Screen screen;
        ASSERT_TRUE(screen.read());
        // A at 1 s: frame 1 of 4 (x 0.5), phase 0.25; B (once, 4 frames) at phase 0.25: frame
        // 0.75 (x 0.375); half each
        EXPECT_NEAR(rowInk(screen, 0.0f).x0, toScreenX(-0.5f + 0.4375f), 1.5f);
    }
}

TEST_F(VatRenderTest, TangentFrameNormalsLightLikeATurnedSurface){
    // every vertex's tangent frame turned 30 degrees about y (positions unchanged): the front
    // face shades as the front face of a box turned 30 degrees, lit by the same light
    const float angle = 30.0f * 3.14159265f / 180.0f;
    VatGltf box;
    box.color[0] = box.color[1] = box.color[2] = 0.8f;
    box.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
    const Vector4 turn = axisAngle(Vector3(0, 1, 0), angle);
    const auto asset = makeAsset(vat::VatMode::Vertices, box.vertexCount(), true, {{"Turn", 1}}, [&box, &turn](int, int vertex, int k){
        if (k == 0)
            return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        const Vector3 n(box.normals[vertex * 3], box.normals[vertex * 3 + 1], box.normals[vertex * 3 + 2]);
        const Vector3 t(box.tangents[vertex * 4], box.tangents[vertex * 4 + 1], box.tangents[vertex * 4 + 2]);
        return multiply(turn, frameQuaternion(t, n.crossProduct(t), n));
    });
    ASSERT_TRUE(asset);

    enum class Kind { ANIMATED, TURNED, PLAIN };
    // the built-in Lit (mesh.vert) and a library shader (includes/surface_vs.glsl)
    for (const char* shader : {"", "library:simple_lit"}){
        SCOPED_TRACE(*shader ? shader : "Lit");
        auto render = [&](Kind kind, Screen& screen){
            VatScene s;
            s.scene().setGlobalIllumination(0.1f);
            Light& light = s.fixture.add<Light>();
            light.setType(LightType::DIRECTIONAL);
            light.setDirection(0.5f, -0.3f, -1.0f);
            light.setIntensity(3.0f);
            Model& model = s.addModel(writeGltf(box, "normals"));
            if (*shader)
                model.setCustomShader(shader);
            if (kind == Kind::ANIMATED)
                s.animate(model.getEntity(), asset, track(0), 0.0f);
            else if (kind == Kind::TURNED)
                model.setRotation(0.0f, 30.0f, 0.0f);
            ASSERT_TRUE(s.settle());
            ASSERT_TRUE(screen.read());
        };
        Screen animated, turned, plain;
        render(Kind::ANIMATED, animated);
        render(Kind::TURNED, turned);
        render(Kind::PLAIN, plain);

        const int x = int(toScreenX(0.0f));
        const int row = toScreenRow(0.2f);
        const uint8_t* a = animated.at(x, row);
        const uint8_t* b = turned.at(x, row);
        const uint8_t* f = plain.at(x, row);
        for (int c = 0; c < 3; c++){
            EXPECT_NEAR(int(a[c]), int(b[c]), 2) << "channel " << c;
        }
        EXPECT_GT(std::abs(int(f[0]) - int(a[0])), 2) << "the turned frame changes the shading";
    }
}

TEST_F(VatRenderTest, UnlitGltfDrawsItsBaseColourWhateverTheLights){
    // a glTF material with KHR_materials_unlit loads as Lighting None: the built-in Lit draws it
    // in its base colour, lit by nothing (a strong light and ambient leave it as a scene without
    // lights draws it), while the same material without the extension is lit; a plain glTF and
    // a .vat.glb playing its animation alike (the VAT variant with MATERIAL_UNLIT)
    VatGltf box;
    box.color[0] = 0.6f; box.color[1] = 0.3f; box.color[2] = 0.15f;
    box.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
    const auto asset = makeMoveAsset(box);
    ASSERT_TRUE(asset);

    // the colour at the box's centre (moved up 1 when animated: the Up clip)
    auto render = [&](bool unlit, bool lights, bool vat, const char* name){
        VatGltf mesh = box;
        mesh.unlit = unlit;
        mesh.extension = vat;
        VatScene s;
        if (lights){
            s.scene().setGlobalIllumination(0.6f);
            Light& light = s.fixture.add<Light>();
            light.setType(LightType::DIRECTIONAL);
            light.setDirection(0.3f, -0.2f, -1.0f);
            light.setIntensity(4.0f);
        }
        Model& model = s.addModel(writeGltf(mesh, name));
        if (vat)
            s.animate(model.getEntity(), asset, track(2), 0.0f);
        EXPECT_TRUE(s.settle());
        const MeshComponent& component = model.getComponent<MeshComponent>();
        EXPECT_EQ(component.submeshes[0].material.lighting, unlit ? MaterialLighting::NONE : MaterialLighting::AUTO) << name;
        Screen screen;
        EXPECT_TRUE(screen.read());
        const uint8_t* p = screen.at(int(toScreenX(0.0f)), toScreenRow(vat ? 1.0f : 0.0f));
        if (vat){
            // the animation moved it: its old bottom edge (y -1) is background now
            EXPECT_TRUE(screen.isBackground(int(toScreenX(0.0f)), toScreenRow(-0.8f))) << name;
        }
        return std::vector<int>{p[0], p[1], p[2]};
    };

    for (bool vat : {false, true}){
        SCOPED_TRACE(vat ? ".vat.glb playing" : "plain glTF");
        const std::string tag = vat ? "unlit_vat" : "unlit_plain";
        const std::vector<int> reference = render(false, false, vat, (tag + "_reference").c_str());  // no lights: Lit draws unlit
        const std::vector<int> unlitLit = render(true, true, vat, (tag + "_lights").c_str());
        const std::vector<int> unlitDark = render(true, false, vat, (tag + "_dark").c_str());
        const std::vector<int> lit = render(false, true, vat, (tag + "_lit").c_str());
        int litDifference = 0;
        for (int c = 0; c < 3; c++){
            EXPECT_NEAR(unlitLit[c], reference[c], 2) << "channel " << c << ": the lights leave it as the base colour";
            EXPECT_NEAR(unlitDark[c], reference[c], 2) << "channel " << c;
            litDifference = std::max(litDifference, std::abs(lit[c] - reference[c]));
        }
        EXPECT_GT(litDifference, 20) << "without the extension the same lights light it";
        // the base colour, red over green over blue
        EXPECT_GT(unlitLit[0], unlitLit[1]);
        EXPECT_GT(unlitLit[1], unlitLit[2]);
    }
}

TEST_F(VatRenderTest, BonesModeSkinsRigidPiecesWithoutShrinkingASpin){
    // left piece (bone 0) rises 0.25 per frame; right piece (bone 1) spins 90 degrees per frame
    // about its own centre (1.5, 0): halfway between two frames its rotation is nlerped to 45
    // degrees and its translation lerped (so its centre swings off the pivot, as the format
    // samples it), and it keeps its size (lerping matrices would shrink it to 0.71)
    VatGltf pieces;
    pieces.addBox(Vector3(-1.5f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f), 0);
    pieces.addBox(Vector3(1.5f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f), 1);
    const float quarter = 3.14159265f * 0.5f;
    const Vector3 pivot(1.5f, 0.0f, 0.0f);
    const auto asset = makeAsset(vat::VatMode::Bones, 2, false, {{"Spin", 4, 1.0f, ClipWrapMode::Loop, true}},
                                 [&](int frame, int bone, int k){
        if (bone == 0)
            return k == 0 ? Vector4(0, 0, 0, 1) : Vector4(0.0f, 0.25f * frame, 0.0f, 1.0f);
        const Vector4 q = axisAngle(Vector3(0, 0, 1), quarter * frame);
        if (k == 0)
            return q;
        const Vector3 translation = pivot - rotate(q, pivot);
        return Vector4(translation.x, translation.y, translation.z, 1.0f);
    });
    ASSERT_TRUE(asset);
    VatScene s;
    Model& model = s.addModel(writeGltf(pieces, "bones"));
    s.animate(model.getEntity(), asset, track(0), 0.0f);
    ASSERT_TRUE(s.settle());
    const MeshComponent& mesh = model.getComponent<MeshComponent>();
    EXPECT_EQ(mesh.vat.variantActive, VatVariant::BONES);
    EXPECT_NE(mesh.submeshes[0].shaderProperties & ShaderPool::getVatProperty(ShaderType::MESH, true), 0u) << "'Vab'";
    EXPECT_EQ(mesh.submeshes[0].shaderProperties & (shaderPropertyBit(13) | ShaderPool::getVatProperty(ShaderType::MESH, false)), 0u) << "no 'Ski', no 'Vat'";

    const int middle = screenWidth / 2;
    for (float now : {0.0f, 0.5f, 1.0f, 2.5f, 3.5f}){
        SCOPED_TRACE("t " + std::to_string(now));
        s.animate(model.getEntity(), asset, track(0), now);
        s.draw();
        Screen screen;
        ASSERT_TRUE(screen.read());
        const float rise = 0.25f * (now < 3.0f ? now : 3.0f * (4.0f - now));  // the seam goes back to 0
        EXPECT_NEAR(columnInk(screen, -1.5f, 0, screenHeight).y0, toScreenY(0.5f + rise), 1.5f) << "left piece";
        // the right piece: rotation nlerped, translation lerped between the two frames
        const float x = std::fmod(now, 4.0f);
        const int f0 = int(std::floor(x));
        const int f1 = (f0 + 1) % 4;
        const float alpha = x - float(f0);
        Vector4 q0 = axisAngle(Vector3(0, 0, 1), quarter * f0);
        Vector4 q1 = axisAngle(Vector3(0, 0, 1), quarter * f1);
        if (q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w < 0.0f)
            q1 = q1 * -1.0f;
        Vector4 q = q0 * (1.0f - alpha) + q1 * alpha;
        q = q / std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        const Vector3 translation = (pivot - rotate(q0, pivot)) * (1.0f - alpha) + (pivot - rotate(axisAngle(Vector3(0, 0, 1), quarter * f1), pivot)) * alpha;
        const Vector3 centre = rotate(q, pivot) + translation;
        // its outline: a unit square, sqrt(2) wide at 45 degrees
        const float turned = std::fmod(now, 1.0f) == 0.5f ? 1.41421356f : 1.0f;
        const InkBox right = screen.inkOnBackground({middle, 0, screenWidth, screenHeight}, 24);
        EXPECT_NEAR(right.width(), turned * pixelsPerUnit, 2.0f) << "right piece: no shrink";
        EXPECT_NEAR(right.height(), turned * pixelsPerUnit, 2.0f);
        EXPECT_NEAR(right.centerX(), toScreenX(centre.x) - 0.5f, 1.5f);
        EXPECT_NEAR((right.y0 + right.y1) * 0.5f, toScreenY(centre.y) - 0.5f, 1.5f);
    }
}

TEST_F(VatRenderTest, BonesModeReadsMoreBonesThanSkinningHas){
    // 300 weight-1 pieces (a rigid destruction's pieces), one bone each, joints as
    // UNSIGNED_SHORT: past the 128 of skinning, every piece reads its own bone (a pattern of
    // them lifted by 0.15)
    const int pieces = 300;
    const int columns = 20;
    const float cellWidth = 0.6f;
    const float cellHeight = 0.45f;
    auto centre = [&](int b){
        return Vector3(-6.0f + cellWidth * (0.5f + float(b % columns)), 3.3f - cellHeight * (0.5f + float(b / columns)), 0.0f);
    };
    auto lifted = [](int b){ return (b * 37) % 5 < 2; };
    VatGltf mesh;
    mesh.shortJoints = true;
    for (int b = 0; b < pieces; b++)
        mesh.addBox(centre(b), Vector3(0.1f, 0.1f, 0.1f), b);
    mesh.vatIndex.clear();      // Bones mode reads the joints only
    const auto asset = makeAsset(vat::VatMode::Bones, pieces, false, {{"Lift", 1}}, [&](int, int bone, int k){
        if (k == 0)
            return Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        return Vector4(0.0f, lifted(bone) ? 0.15f : 0.0f, 0.0f, 1.0f);
    });
    ASSERT_TRUE(asset);
    VatScene s;
    Model& model = s.addModel(writeGltf(mesh, "many_bones"));
    ASSERT_TRUE(s.settle());
    s.animate(model.getEntity(), asset, track(0), 0.0f);
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(model.getComponent<MeshComponent>().vat.variantActive, VatVariant::BONES);

    Screen screen;
    ASSERT_TRUE(screen.read());
    int wrong = 0;
    for (int b = 0; b < pieces; b++){
        const Vector3 c = centre(b);
        const int y0 = int(toScreenY(c.y + cellHeight * 0.5f));
        const int y1 = int(toScreenY(c.y - cellHeight * 0.5f));
        const InkBox ink = columnInk(screen, c.x, y0, y1);
        const float top = c.y + 0.1f + (lifted(b) ? 0.15f : 0.0f);
        if (ink.empty() || std::abs(float(ink.y0) - toScreenY(top)) > 1.5f){
            ADD_FAILURE() << "piece " << b << " (lifted " << lifted(b) << ") top at row " << (ink.empty() ? -1 : ink.y0)
                          << ", expected " << toScreenY(top);
            if (++wrong > 10)
                break;
        }
    }
}

TEST_F(VatRenderTest, InstancesPlayTheirOwnStateAfterSorting){
    // three instances listed far to near (the main camera draws them nearest first), each at its
    // own time of Loop, a fourth playing Up: every one moves by its own offset
    VatGltf box;
    box.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.4f, 0.4f, 0.4f));
    const auto asset = makeMoveAsset(box);
    ASSERT_TRUE(asset);
    VatScene s;
    Model& plainModel = s.addModel(writeGltf(box, "instances_plain"));
    plainModel.createInstancedMesh();
    plainModel.addInstance(-5.0f, -2.5f, 0.0f);
    Model& model = s.addModel(writeGltf(box, "instances"));
    model.createInstancedMesh();
    const float xs[4] = {-4.5f, -1.5f, 1.5f, 4.5f};
    const float zs[4] = {-3.0f, -2.0f, -1.0f, 0.0f};
    for (int i = 0; i < 4; i++)
        model.addInstance(xs[i], 0.0f, zs[i]);
    ASSERT_TRUE(s.settle());
    s.animate(model.getEntity(), asset, track(-1), 0.5f);
    InstancedMeshComponent& instmesh = model.getComponent<InstancedMeshComponent>();
    for (int i = 0; i < 4; i++){
        // instance i at t = 0.5 + i (offsets 0.25, 0.75, 1.25, and Up)
        s.animateInstance(model.getEntity(), i, track(i < 3 ? 0 : 2, -float(i)), 0.5f);
    }
    ASSERT_TRUE(s.settle());

    EXPECT_NE(model.getComponent<MeshComponent>().submeshes[0].shaderProperties & shaderPropertyBit(18), 0u) << "'Ist'";
    EXPECT_TRUE(instmesh.vatBuffer.getRender()->isCreated()) << "the VAT batch has its state buffer";
    EXPECT_EQ(instmesh.vatBuffer.getStride(), 8 * sizeof(float));
    EXPECT_EQ(instmesh.buffer.getStride(), 24 * sizeof(float)) << "its instance buffer keeps its layout";
    InstancedMeshComponent& plain = plainModel.getComponent<InstancedMeshComponent>();
    EXPECT_FALSE(plain.vatBuffer.getRender()->isCreated()) << "another batch has none";
    EXPECT_EQ(plain.buffer.getStride(), 24 * sizeof(float));
    EXPECT_TRUE(plain.renderVat.empty());

    Screen screen;
    ASSERT_TRUE(screen.read());
    const float offsets[3] = {0.25f, 0.75f, 1.25f};
    for (int i = 0; i < 3; i++){
        const int x0 = int(toScreenX(xs[i] - 1.0f));
        const int x1 = int(toScreenX(xs[i] + 1.9f));
        EXPECT_NEAR(rowInk(screen, 0.0f, x0, x1).x0, toScreenX(xs[i] - 0.4f + offsets[i]), 1.5f) << "instance " << i;
    }
    EXPECT_NEAR(columnInk(screen, xs[3], 0, screenHeight).y0, toScreenY(0.4f + 1.0f), 1.5f) << "instance 3 plays Up";

    // the batch drawn back to front (blended): the list sorted by depth, the states with it
    model.getComponent<MeshComponent>().transparent = true;
    model.getComponent<MeshComponent>().autoTransparency = false;
    instmesh.needUpdateInstances = true;
    s.draw();
    Screen sorted;
    ASSERT_TRUE(sorted.read());
    for (int i = 0; i < 3; i++){
        const int x0 = int(toScreenX(xs[i] - 1.0f));
        const int x1 = int(toScreenX(xs[i] + 1.9f));
        EXPECT_NEAR(rowInk(sorted, 0.0f, x0, x1).x0, toScreenX(xs[i] - 0.4f + offsets[i]), 1.5f) << "sorted instance " << i;
    }
}

TEST_F(VatRenderTest, ShadowsAndTheDepthPrePassFollow){
    // a wall behind the box takes its shadow from a light leaning to +x; the box moved 0.6 by
    // its animation moves its shadow by as much, and the depth pre-pass draws what the colour
    // pass draws (depth.vert evaluates the same animation)
    VatGltf box;
    box.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
    const auto asset = makeAsset(vat::VatMode::Vertices, box.vertexCount(), false, {{"Shift", 1}}, [](int, int, int){
        return Vector4(0.6f, 0.0f, 0.0f, 0.0f);
    });
    ASSERT_TRUE(asset);
    // the built-in Lit and depth.vert, and a library shader with its depth program
    // (includes/surface_vs.glsl, includes/surface_depth_vs.glsl)
    for (const char* shader : {"", "library:unlit"}){
        SCOPED_TRACE(*shader ? shader : "Lit");
        auto build = [&](VatScene& s, bool animated, bool prepass){
            s.scene().setDepthPrepassEnabled(prepass);
            s.scene().setGlobalIllumination(0.2f);
            Light& light = s.fixture.add<Light>();
            light.setType(LightType::DIRECTIONAL);
            light.setDirection(0.3f, 0.0f, -1.0f);
            light.setIntensity(4.0f);
            light.setShadows(true);
            Primitive& wall = s.fixture.add<Primitive>();
            wall.createWall(14.0f, 8.0f);
            wall.setPosition(0.0f, 0.0f, -3.0f);
            Model& model = s.addModel(writeGltf(box, "shadow"));
            if (*shader)
                model.setCustomShader(shader);
            if (animated)
                s.animate(model.getEntity(), asset, track(0), 0.0f);
        };
        auto shadowRightEdge = [](const Screen& screen, float worldY, int x0, int litX){
            const int row = toScreenRow(worldY);
            auto luminance = [&](int x){
                const uint8_t* p = screen.at(x, row);
                return int(p[0]) + int(p[1]) + int(p[2]);
            };
            const int lit = luminance(litX);
            int dark = lit;
            for (int x = x0; x < litX; x++)
                dark = std::min(dark, luminance(x));
            const int threshold = (lit + dark) / 2;
            for (int x = litX; x >= x0; x--){
                if (luminance(x) < threshold)
                    return x;
            }
            return -1;
        };

        Screen plain, moved, prepass;
        {
            VatScene s;
            build(s, false, false);
            ASSERT_TRUE(s.settle());
            ASSERT_TRUE(plain.read());
        }
        {
            VatScene s;
            build(s, true, false);
            ASSERT_TRUE(s.settle());
            ASSERT_TRUE(moved.read());
        }
        {
            VatScene s;
            build(s, true, true);
            ASSERT_TRUE(s.settle());
            ASSERT_TRUE(prepass.read());
        }
        const int litX = screenWidth - 4;
        // the box's front face spans x -1..1 (moved: -0.4..1.6); its shadow's right end is the back
        // edge moved 0.3 per unit of depth (2 from the back face to the wall)
        const int plainEdge = shadowRightEdge(plain, 0.5f, int(toScreenX(1.0f)) + 3, litX);
        const int movedEdge = shadowRightEdge(moved, 0.5f, int(toScreenX(1.6f)) + 3, litX);
        ASSERT_GT(plainEdge, 0);
        ASSERT_GT(movedEdge, 0);
        EXPECT_NEAR(movedEdge - plainEdge, 0.6f * pixelsPerUnit, 3.0f) << "the shadow map is drawn animated";
        int differing = 0;
        EXPECT_TRUE(sameImage(moved, prepass, differing)) << differing << " pixels differ with the depth pre-pass";
    }
}

TEST_F(VatRenderTest, AnimatedBoundsKeepAMovedMeshFromBeingCulled){
    // the mesh stands at x 8, out of the view; its animation moves it back by 8 to the centre.
    // An asset without bounds leaves it culled by its rest bounds; with its bounds over the
    // motion the animator sets animatedBounds and it is drawn, single and instanced
    VatGltf box;
    box.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
    auto back = [](int, int, int){ return Vector4(-8.0f, 0.0f, 0.0f, 0.0f); };
    const auto unbounded = makeAsset(vat::VatMode::Vertices, box.vertexCount(), false, {{"Back", 1}}, back);
    vat::VatBounds motion;
    motion.extend(Vector3(-8.5f, -0.5f, -0.5f));
    motion.extend(Vector3(0.5f, 0.5f, 0.5f));
    const auto bounded = makeAsset(vat::VatMode::Vertices, box.vertexCount(), false, {{"Back", 1}}, back, motion);
    ASSERT_TRUE(unbounded);
    ASSERT_TRUE(bounded);
    for (bool instanced : {false, true}){
        SCOPED_TRACE(instanced ? "instanced" : "single");
        VatScene s;
        Model& model = s.addModel(writeGltf(box, instanced ? "bounds_instanced" : "bounds"));
        if (instanced){
            model.createInstancedMesh();
            model.addInstance(8.0f, 0.0f, 0.0f);
        }else{
            model.setPosition(8.0f, 0.0f, 0.0f);
        }
        ASSERT_TRUE(s.settle());
        s.animate(model.getEntity(), unbounded, track(0), 0.0f);
        if (instanced)
            s.animateInstance(model.getEntity(), 0, track(0), 0.0f);
        ASSERT_TRUE(s.settle());
        EXPECT_TRUE(model.getComponent<MeshComponent>().animatedBounds.isNull());
        Screen culled;
        ASSERT_TRUE(culled.read());
        EXPECT_TRUE(rowInk(culled, 0.0f).empty()) << "culled by the rest bounds";

        s.animate(model.getEntity(), bounded, track(0), 0.0f);
        if (instanced)
            s.animateInstance(model.getEntity(), 0, track(0), 0.0f);
        ASSERT_TRUE(s.settle());
        const MeshComponent& mesh = model.getComponent<MeshComponent>();
        EXPECT_EQ(mesh.animatedBounds, AABB(Vector3(-8.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f)));
        Screen drawn;
        ASSERT_TRUE(drawn.read());
        EXPECT_NEAR(rowInk(drawn, 0.0f).x0, toScreenX(-0.5f), 1.5f) << "drawn with the animated bounds";
        EXPECT_LT(mesh.worldAABB.getMinimum().x, 0.0f) << "the world AABB holds the motion";
    }
}

// The shader's frame math against the C++ one (vat::vatLocalTime, vatSample, vatSampleNormalized,
// vatWeightB): rows of small instanced quads, each at its own time, clip, wrap and speed (held
// tracks at speed 0 too, and crossfades and blends of two clips), drawn where the payload's offsets of the frames the C++
// picks, mixed by its alpha and B weight, put them. The offsets differ from frame to frame by
// at least 25 pixels, so a wrong frame or a wrong weight shows.
TEST_F(VatRenderTest, ShaderFrameMathMatchesTheCpp){
    VatGltf quad;
    quad.addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.05f, 0.05f, 0.05f));
    const std::vector<ClipSpec> clips = {
        {"Loop", 7, 3.0f, ClipWrapMode::Loop, true},
        {"Once", 5, 2.0f, ClipWrapMode::Once, true},
        {"PingPong", 6, 4.0f, ClipWrapMode::PingPong, true},
        {"Clamp", 3, 1.0f, ClipWrapMode::ClampForever, true},
        {"Stepped", 4, 2.5f, ClipWrapMode::Loop, false},
        {"Single", 1, 1.0f, ClipWrapMode::Loop, true},
        {"Far", 1, 1.0f, ClipWrapMode::Loop, true},
    };
    // the x offset of global frame g: distinct per frame, steps of 0.5 units (25 pixels)
    auto offsetOf = [](int g){
        if (g == 26)
            return 0.0f;    // Single
        if (g == 27)
            return 3.0f;    // Far
        return float((g * 5) % 7) * 0.5f - 1.5f;
    };
    const auto asset = makeAsset(vat::VatMode::Vertices, quad.vertexCount(), false, clips, [&](int frame, int, int){
        return Vector4(offsetOf(frame), 0.0f, 0.0f, 0.0f);
    });
    ASSERT_TRUE(asset);
    const vat::VatData& data = asset->getData();

    // the C++ position of a track pair at `now`
    auto expected = [&](const vat::VatTrackPair& pair, float now){
        auto value = [&](const vat::VatFrameSample& f){
            return offsetOf(f.frame0) * (1.0f - f.alpha) + offsetOf(f.frame1) * f.alpha;
        };
        float a = 0.0f;
        vat::VatFrameSample sampleA;
        if (pair.a.clip >= 0){
            const vat::VatClip& clip = data.clips[pair.a.clip];
            sampleA = vat::vatSample(clip, vat::vatLocalTime(pair.a, now));
            a = value(sampleA);
        }
        const float w = vat::vatWeightB(pair, now);
        if (w <= 0.0f)
            return a;
        const vat::VatClip& clipB = data.clips[pair.b.clip];
        const vat::VatFrameSample sampleB = (pair.blend >= 0.0f) ? vat::vatSampleNormalized(clipB, sampleA.normalized)
                                                                 : vat::vatSample(clipB, vat::vatLocalTime(pair.b, now));
        return a * (1.0f - w) + value(sampleB) * w;
    };

    // the cases: (pair, now)
    std::vector<std::pair<vat::VatTrackPair, float>> cases;
    const float speeds[] = {1.0f, -1.0f, 1.7f};
    for (int clip = 0; clip < 5; clip++){
        for (float speed : speeds){
            for (float now = -3.1f; now < 6.0f; now += 0.83f)
                cases.push_back({track(clip, 0.2f, speed), now});
        }
    }
    // held (speed 0): the start is the local time, whatever the clock
    for (int clip = 0; clip < 5; clip++){
        for (float held : {-0.7f, 0.0f, 0.35f, 1.3f, 2.9f}){
            for (float now : {-2.0f, 1.7f})
                cases.push_back({track(clip, held, 0.0f), now});
        }
    }
    for (float now : {0.4f, 1.5f, 2.2f, 3.5f}){
        // a crossfade into a held clip, and out of one
        vat::VatTrackPair into = track(0, 0.0f, 1.0f);
        into.b.clip = 2;
        into.b.start = 0.6f;
        into.b.speed = 0.0f;
        into.fadeStart = 1.0f;
        into.fadeEnd = 3.0f;
        cases.push_back({into, now});
        vat::VatTrackPair from = track(2, 0.85f, 0.0f);
        from.b.clip = 0;
        from.b.start = 0.5f;
        from.b.speed = 1.0f;
        from.fadeStart = 1.0f;
        from.fadeEnd = 3.0f;
        cases.push_back({from, now});
    }
    for (float now : {-1.0f, 0.0f, 0.4f, 1.0f, 1.5f, 2.2f, 2.9f, 3.0f, 4.0f}){
        vat::VatTrackPair fade = track(5);
        fade.b.clip = 6;
        fade.fadeStart = 1.0f;
        fade.fadeEnd = 3.0f;
        cases.push_back({fade, now});
        vat::VatTrackPair instant = fade;
        instant.fadeEnd = instant.fadeStart;   // no fade time: B at once
        cases.push_back({instant, now});
    }
    for (float blend : {0.0f, 0.3f, 0.75f, 1.0f}){
        for (float now : {0.3f, 1.1f, 2.6f}){
            vat::VatTrackPair pair = track(0, 0.0f, 1.0f);
            pair.b.clip = 2;
            pair.blend = blend;
            cases.push_back({pair, now});
            vat::VatTrackPair far = track(5);
            far.b.clip = 6;
            far.blend = blend;
            cases.push_back({far, now});
        }
    }

    // 24 rows a draw, a quad per row at x 0 (moved by its offset in [-1.5, 3])
    const int rows = 24;
    const float top = 3.0f;
    const float pitch = 0.25f;
    VatScene s;
    Model& model = s.addModel(writeGltf(quad, "parity"));
    model.createInstancedMesh();
    for (int r = 0; r < rows; r++)
        model.addInstance(0.0f, top - pitch * r, 0.0f);
    ASSERT_TRUE(s.settle());
    s.animate(model.getEntity(), asset, track(-1), 0.0f);
    ASSERT_TRUE(s.settle());
    InstancedMeshComponent& instmesh = model.getComponent<InstancedMeshComponent>();

    // the instances' clock is the entity's (0): each row's own time goes in its times
    int checked = 0;
    for (size_t first = 0; first < cases.size(); first += rows){
        const size_t count = std::min(cases.size() - first, size_t(rows));
        for (int r = 0; r < rows; r++){
            if (size_t(r) < count)
                s.animateInstance(model.getEntity(), r, cases[first + r].first, cases[first + r].second);
            else
                s.animateInstance(model.getEntity(), r, track(-1), 0.0f);
        }
        s.draw();
        Screen screen;
        ASSERT_TRUE(screen.read());
        for (size_t r = 0; r < count; r++){
            const auto& c = cases[first + r];
            const vat::VatTrackPair pair = c.first;
            // what the instance carries (its times moved by -now), at the entity's clock 0
            const float x = expected(vat::vatUnpackInstance(instmesh.instances[r].vat0, instmesh.instances[r].vat1), 0.0f);
            EXPECT_NEAR(x, expected(pair, c.second), 1e-4f) << "the packing keeps the case, " << first + r;
            const InkBox ink = rowInk(screen, top - pitch * float(r));
            ASSERT_FALSE(ink.empty()) << "case " << first + r;
            EXPECT_NEAR(ink.x0, toScreenX(x - 0.05f), 1.0f)
                << "case " << first + r << ": clip " << pair.a.clip << " speed " << pair.a.speed << " at " << c.second
                << ", B " << pair.b.clip << " blend " << pair.blend << " fade " << pair.fadeStart << ".." << pair.fadeEnd;
            checked++;
        }
    }
    EXPECT_GT(checked, 200);
}
