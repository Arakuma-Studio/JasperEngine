// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatCrowd.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "htmlui/UIWorldRaycast.h"
#include "math/Ray.h"
#include "subsystem/MeshSystem.h"
#include "vat/VatSceneActions.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

using namespace jasperengine;

namespace fs = std::filesystem;
namespace ev = jasperengine::editor::vat;

namespace{

    constexpr double PI = 3.14159265358979323846;

    // splitmix64: the same numbers on every platform and compiler
    struct Random{
        uint64_t state;
        explicit Random(uint64_t seed): state(seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull){}
        uint64_t next(){
            uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        // [0, 1)
        double unit(){ return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
        double range(double low, double high){ return low + (high - low) * unit(); }
    };

    // the spacing check's grid: cells of the minimum spacing, the 3x3 around a point
    struct SpacingGrid{
        double cell = 1.0;
        std::unordered_map<int64_t, std::vector<std::pair<double, double>>> cells;

        static int64_t key(int64_t x, int64_t z){ return (x << 32) ^ (z & 0xFFFFFFFFll); }
        int64_t index(double v) const{ return (int64_t)std::floor(v / cell); }

        bool free(double x, double z, double spacing) const{
            const int64_t cx = index(x), cz = index(z);
            for (int64_t dx = -1; dx <= 1; dx++){
                for (int64_t dz = -1; dz <= 1; dz++){
                    auto it = cells.find(key(cx + dx, cz + dz));
                    if (it == cells.end()) continue;
                    for (const auto& p : it->second){
                        const double ex = p.first - x, ez = p.second - z;
                        if (ex * ex + ez * ez < spacing * spacing) return false;
                    }
                }
            }
            return true;
        }
        void add(double x, double z){ cells[key(index(x), index(z))].push_back({x, z}); }
    };

    std::string number(float value){
        char text[32];
        snprintf(text, sizeof(text), "%g", value);
        return text;
    }

}

ev::CrowdLayout ev::layoutCrowd(const CrowdSettings& settings, const std::function<bool(float x, float z, float& y)>& groundHeight){
    CrowdLayout layout;
    layout.requested = std::max(settings.count, 0);
    Random random(((uint64_t)settings.seed << 1) | 1u);

    // the weighted clip table (cumulative)
    std::vector<std::pair<double, std::string>> table;
    double total = 0.0;
    for (const CrowdClipWeight& weight : settings.clipWeights){
        if (!(weight.weight > 0.0f)) continue;
        total += weight.weight;
        table.push_back({total, weight.clip});
    }

    const double spacing = std::max(0.0f, settings.minSpacing);
    SpacingGrid grid;
    grid.cell = spacing > 0.0 ? spacing : 1.0;
    const int tries = std::max(settings.triesPerInstance, 1);
    const double halfWidth = std::max(0.0f, settings.width) * 0.5;
    const double halfDepth = std::max(0.0f, settings.depth) * 0.5;

    for (int i = 0; i < layout.requested; i++){
        bool found = false;
        double x = 0.0, z = 0.0;
        for (int t = 0; t < tries && !found; t++){
            const double u = random.unit(), v = random.unit();
            if (settings.area == CrowdArea::Disc){
                const double radius = halfWidth * std::sqrt(u);
                const double angle = 2.0 * PI * v;
                x = radius * std::cos(angle);
                z = radius * std::sin(angle);
            }else{
                x = (u - 0.5) * 2.0 * halfWidth;
                z = (v - 0.5) * 2.0 * halfDepth;
            }
            found = spacing <= 0.0 || grid.free(x, z, spacing);
        }
        // every instance draws the same numbers whether it is placed or not: one skipped instance
        // never changes the others
        const double yaw = random.range(0.0, 2.0 * PI);
        const double scale = random.range(settings.scaleMin, settings.scaleMax);
        const double pick = random.unit();
        const double speed = random.range(settings.speedMin, settings.speedMax);
        if (!found) continue;
        if (spacing > 0.0) grid.add(x, z);

        CrowdInstanceSpec instance;
        float groundY = settings.center.y;
        float height = 0.0f;
        if (groundHeight && groundHeight(settings.center.x + (float)x, settings.center.z + (float)z, height)){
            groundY = height;
        }
        instance.position = Vector3((float)x, groundY - settings.center.y, (float)z);
        if (settings.randomYaw){
            const double half = yaw * 0.5;
            instance.rotation = Quaternion((float)std::cos(half), 0.0f, (float)std::sin(half), 0.0f);
        }
        instance.scale = Vector3((float)scale, (float)scale, (float)scale);
        if (!table.empty()){
            const double at = pick * total;
            auto it = std::upper_bound(table.begin(), table.end(), at, [](double value, const std::pair<double, std::string>& entry){
                return value < entry.first;
            });
            instance.clip = (it == table.end() ? table.back() : *it).second;
        }
        instance.speed = (float)speed;
        layout.instances.push_back(std::move(instance));
    }
    layout.placed = (int)layout.instances.size();
    return layout;
}

std::string ev::validateCrowd(const CrowdSettings& settings, const std::vector<std::string>& clipNames){
    if (settings.count < 1 || settings.count > MAX_CROWD){
        return "count must be 1.." + std::to_string(MAX_CROWD) + " (got " + std::to_string(settings.count) + ")";
    }
    if (!std::isfinite(settings.width) || !std::isfinite(settings.depth) || settings.width < 0.0f ||
        (settings.area == CrowdArea::Box && settings.depth < 0.0f)){
        return "the area size must be 0 or more";
    }
    if (!std::isfinite(settings.minSpacing) || settings.minSpacing < 0.0f) return "the minimum spacing must be 0 or more";
    if (!(settings.scaleMin > 0.0f) || !std::isfinite(settings.scaleMax) || settings.scaleMax < settings.scaleMin){
        return "the scale range must be above 0 with min <= max (got " + number(settings.scaleMin) + ".." + number(settings.scaleMax) + ")";
    }
    if (!std::isfinite(settings.speedMin) || !std::isfinite(settings.speedMax) || settings.speedMax < settings.speedMin){
        return "the speed range needs min <= max (got " + number(settings.speedMin) + ".." + number(settings.speedMax) + ")";
    }
    if (!settings.clipWeights.empty()){
        bool positive = false;
        std::set<std::string> seen;
        for (const CrowdClipWeight& weight : settings.clipWeights){
            if (std::find(clipNames.begin(), clipNames.end(), weight.clip) == clipNames.end()){
                std::string names;
                for (const std::string& name : clipNames) names += (names.empty() ? "" : ", ") + name;
                return "no clip '" + weight.clip + "' in the Vertex Animation (clips: " + names + ")";
            }
            if (!seen.insert(weight.clip).second) return "clip '" + weight.clip + "' is weighted twice";
            if (!std::isfinite(weight.weight) || weight.weight < 0.0f) return "the weight of '" + weight.clip + "' must be 0 or more";
            positive |= weight.weight > 0.0f;
        }
        if (!positive) return "at least one clip needs a weight above 0";
    }
    return std::string();
}

ev::CrowdResult ev::createCrowd(Project* project, uint32_t sceneId, const CrowdSettings& settings){
    CrowdResult result;
    result.requested = settings.count;
    VatObjectSource source;
    std::string error;
    if (!resolveVatObjectSource(project, settings.vat, source, &error)){
        result.message = error;
        return result;
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        result.message = "no such scene";
        return result;
    }
    if (sceneProject->sceneType != SceneType::SCENE_3D){
        result.message = "a crowd is made in a 3D scene";
        return result;
    }
    std::vector<std::string> clipNames;
    for (const jasperengine::vat::VatClip& clip : source.data.clips) clipNames.push_back(clip.name);
    const std::string problem = validateCrowd(settings, clipNames);
    if (!problem.empty()){
        result.message = problem;
        return result;
    }

    // the ground: the surface's triangles (or terrain), else its bounds' top; else none (center.y)
    Scene* scene = sceneProject->scene;
    Entity surface = settings.surface;
    if (surface != NULL_ENTITY && !scene->isEntityCreated(surface)) surface = NULL_ENTITY;
    std::function<bool(float, float, float&)> ground;
    if (surface != NULL_ENTITY){
        ground = [scene, surface](float x, float z, float& y){
            MeshComponent* mesh = scene->findComponent<MeshComponent>(surface);
            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(surface);
            Transform* transform = scene->findComponent<Transform>(surface);
            float top = 1000.0f, bottom = -1000.0f;
            const AABB* bounds = mesh && !mesh->worldAABB.isNull() && !mesh->worldAABB.isInfinite() ? &mesh->worldAABB : nullptr;
            if (bounds){
                top = bounds->getMaximum().y;
                bottom = bounds->getMinimum().y;
            }
            const Ray ray(Vector3(x, top + 1.0f, z), Vector3(0.0f, -(top - bottom + 2.0f), 0.0f));
            if (terrain && transform){
                Vector3 point;
                if (scene->getSystem<MeshSystem>()->raycastTerrainSurface(ray, *terrain, *transform, point)){
                    y = point.y;
                    return true;
                }
            }
            if (mesh){
                const UIWorldHit hit = raycastMeshUV(scene, surface, ray);
                if (hit.hit){
                    y = top + 1.0f - hit.distance;
                    return true;
                }
                if (bounds && x >= bounds->getMinimum().x && x <= bounds->getMaximum().x &&
                    z >= bounds->getMinimum().z && z <= bounds->getMaximum().z){
                    y = top;
                    return true;
                }
            }
            return false;
        };
    }
    const CrowdLayout layout = layoutCrowd(settings, ground);
    result.placed = layout.placed;
    if (layout.placed == 0){
        result.message = "no instance fits: lower the minimum spacing or make the area larger";
        return result;
    }

    CreateVatObjectCmd::Spec spec;
    spec.name = settings.name.empty() ? source.stem + " Crowd" : settings.name;
    spec.parent = settings.parent;
    spec.position = settings.center;
    spec.mesh = source.mesh;
    spec.animator.animation = source.vat;
    spec.animator.guid = source.guid;
    spec.animator.randomStart = settings.randomStart;
    spec.animator.instanceEvents = settings.instanceEvents;
    spec.instanced = true;
    bool anyClip = false, anySpeed = false;
    for (const CrowdInstanceSpec& instance : layout.instances){
        anyClip |= !instance.clip.empty();
        anySpeed |= instance.speed != 1.0f;
    }
    for (const CrowdInstanceSpec& instance : layout.instances){
        InstanceData data;
        data.position = instance.position;
        data.rotation = instance.rotation;
        data.scale = instance.scale;
        spec.instances.push_back(data);
        if (anyClip) spec.animator.instanceClips.push_back(instance.clip);
        if (anySpeed) spec.animator.instanceSpeeds.push_back(instance.speed);
    }
    auto* command = new CreateVatObjectCmd(project, sceneId, spec);
    command->setNoMerge();
    if (!CommandHandle::get(sceneId)->addCommand(command)){
        result.message = "the crowd could not be created (the mesh " + source.mesh + " did not load)";
        return result;
    }
    result.ok = true;
    result.entity = command->getEntity();
    result.message = "Placed " + std::to_string(layout.placed) + " of " + std::to_string(layout.requested);
    if (layout.placed < layout.requested){
        result.message += " (minimum spacing " + number(settings.minSpacing) + " m left no room for " +
                          std::to_string(layout.requested - layout.placed) + ")";
    }
    return result;
}
