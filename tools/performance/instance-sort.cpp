// CPU-only benchmark for the production render ordering helper, not frame time.
// Thread CPU time excludes scheduler pauses on a busy build host. The baseline is
// the comparator from RenderSystem::sortInstancedMesh before depth caching.
#include "buffer/ExternalBuffer.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector4.h"
#include "component/InstancedMeshComponent.h"
#include "subsystem/InstanceSort.h"
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>

using namespace jasperengine;
using Order = std::vector<unsigned int>;

Order originalOrder(const std::vector<InstanceRenderData>& instances,
                    const Matrix4& model, const Vector3& direction) {
    Order order(instances.size());
    for (unsigned int i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](unsigned int a, unsigned int b) {
        const auto& ma = instances[a].instanceMatrix;
        const auto& mb = instances[b].instanceMatrix;
        Vector3 pa(ma[3][0], ma[3][1], ma[3][2]);
        Vector3 pb(mb[3][0], mb[3][1], mb[3][2]);
        return (model * pa).dotProduct(direction) < (model * pb).dotProduct(direction);
    });
    return order;
}

volatile unsigned long long checksum = 0;
double threadMicros() {
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0)
        throw std::runtime_error("cannot read thread CPU clock");
    return double(time.tv_sec) * 1000000.0 + double(time.tv_nsec) / 1000.0;
}
template <typename Sort>
double measure(Sort sort, const std::vector<InstanceRenderData>& instances,
               const Matrix4& model, const Vector3& direction, int repeats) {
    const double start = threadMicros();
    for (int i = 0; i < repeats; ++i) {
        const auto result = sort(instances, model, direction);
        checksum += result.empty() ? 0 : result[result.size() / 2];
    }
    return (threadMicros() - start) / repeats;
}

int main() {
    std::mt19937 random(48713);
    std::uniform_real_distribution<float> value(-1000.0f, 1000.0f);
    const Vector3 directions[] = {Vector3(0.2f, -0.3f, 0.9f).normalize(), Vector3(0, 0, 1), Vector3::ZERO};
    Matrix4 models[3];
    models[1] = Matrix4(-2, 0.1f, 0.2f, 0, 0.3f, 1, 0.4f, 0, 0.5f, 0.6f, 3, 0, 50, -20, 10, 1);
    models[2] = models[1];
    models[2].set(0, 3, 0.00001f); // Also retain Matrix4's projective divide.
    for (size_t count : {0u, 1u, 2u, 16u, 100u, 1000u, 10000u}) {
        std::vector<InstanceRenderData> instances(count);
        for (int distribution = 0; distribution < 4; ++distribution) {
            for (size_t i = 0; i < count; ++i) {
                Vector3 position(value(random), value(random), value(random));
                if (distribution == 1) position = Vector3(0, 0, float(i));
                if (distribution == 2) position = Vector3(0, 0, float(count - i));
                if (distribution == 3) position = Vector3(float(i % 3), 0, float(i % 7));
                instances[i].instanceMatrix = Matrix4::translateMatrix(position);
            }
            for (const auto& model : models) for (const auto& direction : directions) {
                if (originalOrder(instances, model, direction) != detail::instanceDepthOrder(instances, model, direction))
                    throw std::runtime_error("instance ordering changed");
            }
        }
    }
    std::puts("PASS: exact index-order parity for 252 empty/singleton/random/sorted/reversed/tied transform cases");
    std::puts("instances,baseline_cpu_us,cached_cpu_us,speedup");
    for (size_t count : {100u, 1000u, 10000u, 100000u}) {
        std::vector<InstanceRenderData> instances(count);
        for (auto& instance : instances)
            instance.instanceMatrix = Matrix4::translateMatrix(Vector3(value(random), value(random), value(random)));
        const int repeats = std::max(3, int(100000 / count));
        std::vector<double> before, after;
        for (int trial = 0; trial < 7; ++trial) {
            auto baseline = [&] { return measure(originalOrder, instances, models[1], directions[0], repeats); };
            auto cached = [&] { return measure(detail::instanceDepthOrder<InstanceRenderData>, instances, models[1], directions[0], repeats); };
            // Alternate order to limit temperature/scheduling bias.
            if (trial % 2) { after.push_back(cached()); before.push_back(baseline()); }
            else { before.push_back(baseline()); after.push_back(cached()); }
        }
        std::sort(before.begin(), before.end());
        std::sort(after.begin(), after.end());
        std::printf("%zu,%.3f,%.3f,%.2fx\n", count, before[3], after[3], before[3] / after[3]);
    }
}
