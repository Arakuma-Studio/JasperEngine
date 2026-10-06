// Exact production methods are injected by run_instance_views.py. The fixture
// uses real math and instance layouts, with only the GPU/Scene owners omitted.
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <random>
#include <vector>
#include "math/Matrix4.h"
#include "math/Plane.h"
#include "math/Rect.h"
using namespace jasperengine;
constexpr unsigned MAX_MESH_LODS = 4;
constexpr unsigned FRUSTUM_PLANE_FAR = 1;
// INSTANCE_TYPES
struct InstanceView { std::vector<InstanceRenderData> data; };
struct Transform { Matrix4 modelMatrix; Vector3 worldScale = Vector3(1, 1, 1); };
struct InstancedMeshComponent { bool cullInstances = true; float cullDistance = 0; };
struct MeshComponent {
    bool lodEnabled = true; unsigned numLods = 4; float lodBias = 1;
    float lodError[MAX_MESH_LODS] = {0, 0.1f, 1.f, 10.f};
};
// VIEW_TYPES
#define DECLARE_SYSTEM(Name) \
struct Name { \
    unsigned int selectMeshLod(const MeshComponent&, const LodView*, float, float) const; \
    bool isInsideCamera(float, const Plane[6], const Vector3&, const float&); \
    void buildInstanceView(InstanceView&, InstanceViewRange&, const InstanceRenderData*, const InstanceBounds*, unsigned int, const InstancedMeshComponent&, const MeshComponent&, const Transform&, float, const Plane[6], const LodView&, const Vector3&, const Vector3&, const Vector3&, InstanceOrder, bool); \
};
DECLARE_SYSTEM(Before)
DECLARE_SYSTEM(After)
// BEFORE_METHODS
// AFTER_METHODS
struct Fixture {
    std::vector<InstanceRenderData> instances;
    std::vector<InstanceBounds> bounds;
    InstancedMeshComponent config;
    MeshComponent mesh;
    Transform transform;
    LodView lod;
    Plane planes[6] = {Plane(1,0,0,100), Plane(-1,0,0,100), Plane(0,1,0,100), Plane(0,-1,0,100), Plane(0,0,1,100), Plane(0,0,-1,100)};
    Vector3 eye = Vector3(0, 1, 2);
    Vector3 direction = Vector3(0, 0, 1);
    float farClip = 1000;
    bool limitDistance = true;
    InstanceOrder order = InstanceOrder::KEEP;
    explicit Fixture(unsigned size) : instances(size), bounds(size) {
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> position(-120, 120);
        lod.enabled = true; lod.perUnit = 0.01f;
        transform.worldScale = Vector3(1, 2, -0.5f);
        transform.modelMatrix = Matrix4::scaleMatrix(transform.worldScale);
        for (unsigned i = 0; i < size; ++i) {
            instances[i].color = Vector4(float(i), float(i + 1), float(i + 2), 1);
            instances[i].textureRect = Rect(float(i), 0, 1, 1);
            bounds[i].center = Vector3(position(rng), position(rng), position(rng));
            bounds[i].radius = float(i % 7); bounds[i].scale = 1 + float(i % 3);
        }
    }
    template<class System> void run(System& system, InstanceView& view, InstanceViewRange& range) {
        system.buildInstanceView(view, range, instances.data(), bounds.data(), instances.size(), config, mesh, transform,
            farClip, planes, lod, eye, eye, direction, order, limitDistance);
    }
};
static void check() {
    Before before; After after;
    for (unsigned size : {0u, 1u, 17u, 1000u}) {
        Fixture f(size);
        for (auto order : {InstanceOrder::KEEP, InstanceOrder::NEAREST, InstanceOrder::BACK_TO_FRONT})
        for (bool cull : {false, true})
        for (float distance : {0.f, 75.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        for (bool enabled : {false, true})
        for (bool far : {false, true}) {
            f.order = order; f.config.cullInstances = cull; f.config.cullDistance = distance;
            f.lod.enabled = enabled; f.farClip = far ? 1000 : 0;
            f.limitDistance = enabled; f.direction = enabled ? Vector3::ZERO : Vector3(0,0,1);
            InstanceView a, b; a.data.resize(3); b.data = a.data;
            InstanceViewRange ar, br;
            f.run(before, a, ar); f.run(after, b, br);
            assert(ar.offset == br.offset); assert(ar.count() == br.count());
            for (unsigned l = 0; l < MAX_MESH_LODS; ++l) assert(ar.lodCount[l] == br.lodCount[l]);
            assert(a.data.size() == b.data.size());
            for (size_t i = 0; i < a.data.size(); ++i) {
                assert(a.data[i].instanceMatrix == b.data[i].instanceMatrix);
                assert(a.data[i].color == b.data[i].color);
                assert(a.data[i].textureRect == b.data[i].textureRect);
            }
        }
    }
    std::fprintf(stderr, "instance view equivalence assertions passed (480 cases)\n");
}
volatile unsigned consumed = 0;
template<class System> double timeRun(System& system, Fixture& f, unsigned repeats) {
    InstanceView view; InstanceViewRange range;
    f.run(system, view, range); // warm up scratch and destination capacity
    auto start = std::clock();
    for (unsigned i = 0; i < repeats; ++i) {
        view.data.clear(); f.run(system, view, range); consumed = range.count();
    }
    return 1e9 * double(std::clock() - start) / CLOCKS_PER_SEC / repeats;
}
int main() {
    check();
#ifdef CHECK_ONLY
    return 0;
#endif
    Before before; After after;
    for (unsigned size : {1000u, 10000u, 100000u}) {
        Fixture f(size);
        for (unsigned kind = 0; kind < 4; ++kind) {
            const char* name = kind == 0 ? "keep_unculled" : kind == 1 ? "keep_culled" : kind == 2 ? "nearest" : "back_to_front";
            f.order = kind < 2 ? InstanceOrder::KEEP : kind == 2 ? InstanceOrder::NEAREST : InstanceOrder::BACK_TO_FRONT;
            f.config.cullInstances = kind != 0;
            for (unsigned sample = 0; sample < 7; ++sample) {
                double a, b;
                unsigned repeats = std::max(10u, 1000000u / size);
                if (sample % 2) { b = timeRun(after, f, repeats); a = timeRun(before, f, repeats); }
                else { a = timeRun(before, f, repeats); b = timeRun(after, f, repeats); }
                std::printf("%s,%u,%u,%.1f,%.1f\n", name, size, sample, a, b);
            }
        }
    }
}
