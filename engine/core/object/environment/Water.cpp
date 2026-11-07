// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Water.h"
#include "util/WaterWaves.h"
#include "component/BuoyancyComponent.h"

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "texture/Material.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/SplineSystem.h"
#include "subsystem/WaterInteractionSystem.h"
#include "component/WaterExcluderComponent.h"
#ifdef JASPERENGINE_PHYSICS_3D
#include "component/RigidbodyComponent.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace jasperengine;

const char* const Water::SHADER = "library:water";
const char* const Water::UNDERWATER_SHADER = "library:underwater";

namespace{

    const float WATER_TWO_PI = 6.28318530718f;

    // the camera is under the surface below it, and back above this much over it (the waves
    // would switch the pass on and off every frame at the surface; the pass fades only the
    // part of the view below waterLevel, so a camera just above it still looks right)
    const float UNDERWATER_HYSTERESIS = 0.05f;

    uint32_t hash32(uint32_t x){
        // lowbias32 (Chris Wellons)
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    uint32_t packPixel(float r, float g, float b){
        auto byte = [](float v){
            return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
        };
        return 0xFF000000u | (byte(b) << 16) | (byte(g) << 8) | byte(r);
    }

}

bool WaterSurface::covers(float x, float z) const{
    return x >= minXZ.x && x <= maxXZ.x && z >= minXZ.y && z <= maxXZ.y;
}

float WaterSurface::getBottom() const{
    return buoyancyDepth > 0.0f ? level - buoyancyDepth : -std::numeric_limits<float>::infinity();
}

WaterWaveSample WaterSurface::getWaves(float x, float z, float time) const{
    return waterSurfaceAt(waves, Vector2(x, z), time);
}

float WaterSurface::getHeight(float x, float z, float time) const{
    if (!waves.waves)
        return level;
    return level + getWaves(x, z, time).height;
}

Vector3 WaterSurface::getNormal(float x, float z, float time) const{
    if (!waves.waves)
        return Vector3(0.0f, 1.0f, 0.0f);
    return getWaves(x, z, time).normal;
}

bool Water::isWaterShader(const std::string& reference){
    if (reference.empty())
        return false;
    const std::string base = ShaderReference::getBase(reference);
    if (base == SHADER)
        return true;
    if (ShaderReference::isLibrary(base) || ShaderReference::isBuiltin(base))
        return false;
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(base);
    return manifest && manifest->role == ShaderRole::WATER;
}

bool Water::isUnderwaterShader(const std::string& reference){
    if (reference.empty())
        return false;
    const std::string base = ShaderReference::getBase(reference);
    if (base == UNDERWATER_SHADER)
        return true;
    if (ShaderReference::isLibrary(base) || ShaderReference::isBuiltin(base))
        return false;
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(base);
    return manifest && manifest->role == ShaderRole::UNDERWATER;
}

float Water::getTime(Scene* scene){
    // the scene clock, which RenderSystem writes into library:water's sceneTime
    if (!scene)
        scene = Engine::getMainScene();
    return scene ? (float)scene->getSceneTime() : 0.0f;
}

Vector3 Water::getWaves(float x, float z, float time, const Vector2& direction, float height, float waveLength, float speed){
    // library/water.glsl's waterWaves: its one C++ twin, pinned to the GLSL (WaterWavesTest)
    const WaterWaveSample wave = waterWaves(Vector2(x, z), time, direction, height, waveLength, speed);
    return Vector3(wave.height, wave.slope.x, wave.slope.y);
}

bool Water::readWaves(const Material& material, WaterWaveParams& waves){
    const std::string& shader = material.shader;
    if (!isWaterShader(shader))
        return false;

    // a fork's own manifest gives its defaults
    const ShaderUniformValues& values = material.shaderParams;
    const std::vector<std::string> features = ShaderReference::getEnabledFeatures(shader, material.shaderFeatures);
    waves = WaterWaveParams();
    waves.waves = std::find(features.begin(), features.end(), "WAVES") != features.end();
    waves.gerstner = waves.waves && std::find(features.begin(), features.end(), "GERSTNER") != features.end();
    const Vector4 direction = ShaderLibraryValues::getParam(values, shader, "waveDirection");
    waves.direction = Vector2(direction.x, direction.y);
    waves.height = ShaderLibraryValues::getParam(values, shader, "waveHeight").x;
    waves.length = ShaderLibraryValues::getParam(values, shader, "waveLength").x;
    waves.speed = ShaderLibraryValues::getParam(values, shader, "waveSpeed").x;
    waves.steepness = ShaderLibraryValues::getParam(values, shader, "waveSteepness").x;
    return true;
}

bool Water::readMaterial(const Material& material, WaterSurface& surface){
    if (!readWaves(material, surface.waves))
        return false;

    const std::string& shader = material.shader;
    const ShaderUniformValues& values = material.shaderParams;
    surface.buoyancy = ShaderLibraryValues::getParam(values, shader, "buoyancy").x > 0.5f;
    surface.buoyancyDepth = ShaderLibraryValues::getParam(values, shader, "buoyancyDepth").x;
    surface.shallowColor = ShaderLibraryValues::getParam(values, shader, "shallowColor");
    surface.deepColor = ShaderLibraryValues::getParam(values, shader, "deepColor");
    return true;
}

std::vector<WaterSurface> Water::getSurfaces(Scene* scene){
    std::vector<WaterSurface> surfaces;
    if (!scene)
        return surfaces;

    // spline rivers and lakes answer through SplineSystem::sampleWaterSurface, not their chunks' bounds
    std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>();
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < (int)meshes->size(); i++){
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        if (!mesh.enabled || mesh.aabb.isNull())
            continue;
        if (splines && splines->isGenerated(meshes->getEntity(i)))
            continue;

        WaterSurface surface;
        bool found = false;
        for (unsigned int s = 0; s < mesh.numSubmeshes && !found; s++){
            found = readMaterial(mesh.submeshes[s].material, surface);
        }
        if (!found)
            continue;

        const Entity entity = meshes->getEntity(i);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !scene->isEntityActiveInHierarchy(entity))
            continue;

        const AABB bounds = transform->modelMatrix * mesh.aabb;
        if (bounds.isNull())
            continue;

        surface.entity = entity;
        surface.level = bounds.getMaximum().y;
        surface.minXZ = Vector2(bounds.getMinimum().x, bounds.getMinimum().z);
        surface.maxXZ = Vector2(bounds.getMaximum().x, bounds.getMaximum().z);
        surfaces.push_back(surface);
    }

    return surfaces;
}

const WaterSurface* Water::findSurface(const std::vector<WaterSurface>& surfaces, const Vector3& point, bool buoyant){
    const WaterSurface* found = nullptr;
    float foundBottom = 0.0f;
    for (const WaterSurface& surface : surfaces){
        if ((buoyant && !surface.buoyancy) || !surface.covers(point.x, point.z))
            continue;
        const float bottom = surface.getBottom();
        if (point.y < bottom)
            continue;
        if (!found || bottom > foundBottom || (bottom == foundBottom && surface.level > found->level)){
            found = &surface;
            foundBottom = bottom;
        }
    }
    return found;
}

// a spline river or lake over (x, z) (SplineSystem), before the library:water meshes
static SplineWaterSample splineWater(Scene* scene, float x, float z){
    std::shared_ptr<SplineSystem> splines = scene ? scene->findSystem<SplineSystem>() : nullptr;
    return splines && splines->hasWaterSurfaces() ? splines->sampleWaterSurface(x, z) : SplineWaterSample();
}

std::optional<float> Water::getHeight(Scene* scene, float x, float z){
    const SplineWaterSample water = splineWater(scene, x, z);
    if (water.hit)
        return water.height;
    const std::vector<WaterSurface> surfaces = getSurfaces(scene);
    const WaterSurface* surface = findSurface(surfaces, Vector3(x, std::numeric_limits<float>::infinity(), z));
    if (!surface)
        return std::nullopt;
    return surface->getHeight(x, z, getTime(scene));
}

std::optional<Vector3> Water::getNormal(Scene* scene, float x, float z){
    const SplineWaterSample water = splineWater(scene, x, z);
    if (water.hit)
        return water.normal;
    const std::vector<WaterSurface> surfaces = getSurfaces(scene);
    const WaterSurface* surface = findSurface(surfaces, Vector3(x, std::numeric_limits<float>::infinity(), z));
    if (!surface)
        return std::nullopt;
    return surface->getNormal(x, z, getTime(scene));
}

// inside an active Water Excluder (a boat, a cabin): no water there
static bool waterKeptDry(Scene* scene, const Vector3& point){
    if (!scene || scene->getComponentArray<WaterExcluderComponent>()->size() == 0)
        return false;
    std::shared_ptr<WaterInteractionSystem> interaction = scene->findSystem<WaterInteractionSystem>();
    return interaction && interaction->findExcluder(point) != NULL_ENTITY;
}

bool Water::isUnderwater(Scene* scene, const Vector3& point){
    if (waterKeptDry(scene, point))
        return false;
    const SplineWaterSample water = splineWater(scene, point.x, point.z);
    if (water.hit)
        return point.y < water.height;
    const std::vector<WaterSurface> surfaces = getSurfaces(scene);
    const WaterSurface* surface = findSurface(surfaces, point);
    return surface && point.y < surface->getHeight(point.x, point.z, getTime(scene));
}

WaterSample Water::sample(Scene* scene, const Vector3& point){
    if (!scene)
        return WaterSample();
    return scene->getSystem<PhysicsSystem>()->sampleWater(point);
}

std::optional<float> Water::sampleHeight(Scene* scene, float x, float z){
    if (!scene)
        return std::nullopt;
    const float height = scene->getSystem<PhysicsSystem>()->sampleWaterHeight(x, z);
    if (std::isnan(height))
        return std::nullopt;
    return height;
}

void Water::applyBuoyancy(Scene* scene, float deltaTime){
#ifdef JASPERENGINE_PHYSICS_3D
    auto bodies = scene->getComponentArray<RigidbodyComponent>();

    std::vector<WaterSurface> surfaces;
    bool surfacesRead = false;
    float time = 0.0f;
    std::shared_ptr<WaterInteractionSystem> excluders;
    PhysicsSystem* physics = scene->getSystem<PhysicsSystem>().get();

    for (int i = 0; i < (int)bodies->size(); i++){
        const RigidbodyComponent& body = bodies->getComponentFromIndex(i);
        if (!body.enabled || body.buoyancy <= 0.0f || body.type != BodyType::DYNAMIC || body.body.IsInvalid())
            continue;
        // a Buoyancy floats it by its columns in the Water Volumes instead (WaterSystem)
        if (const BuoyancyComponent* columns = scene->findComponent<BuoyancyComponent>(bodies->getEntity(i)); columns && columns->enabled)
            continue;
        // Jolt has no submerged volume for a mesh or heightfield (it asserts and leaves it unset)
        bool solid = true;
        for (size_t c = 0; c < body.colliders.size() && solid; c++){
            solid = body.colliders[c].type != Collider3DType::MESH && body.colliders[c].type != Collider3DType::HEIGHTFIELD;
        }
        if (!solid)
            continue;

        // the waters, once a step and only with a floating body
        if (!surfacesRead){
            surfacesRead = true;
            surfaces = getSurfaces(scene);
            time = getTime(scene);
            if (scene->getComponentArray<WaterExcluderComponent>()->size() > 0)
                excluders = scene->findSystem<WaterInteractionSystem>();
        }
        if (surfaces.empty())
            return;

        JPH::BodyInterface& bodyInterface = physics->getBodyInterface3D();
        if (!bodyInterface.IsAdded(body.body))
            continue;

        const JPH::RVec3 center = bodyInterface.GetCenterOfMassPosition(body.body);
        const Vector3 point((float)center.GetX(), (float)center.GetY(), (float)center.GetZ());
        const WaterSurface* surface = findSurface(surfaces, point, true);
        if (!surface)
            continue;
        // inside a Water Excluder it does not own (cargo in a boat): dry
        if (excluders && excluders->findExcluder(point, bodies->getEntity(i)) != NULL_ENTITY)
            continue;

        // the surface over the centre of mass, with the waves of the frame about to be drawn
        const float height = surface->getHeight(point.x, point.z, time);
        const Vector3 normal = surface->getNormal(point.x, point.z, time);
        const Vector3 gravity = physics->getGravity3D();
        bodyInterface.ApplyBuoyancyImpulse(body.body, JPH::RVec3(point.x, height, point.z), JPH::Vec3(normal.x, normal.y, normal.z),
            body.buoyancy, body.waterDrag, body.waterAngularDrag, JPH::Vec3::sZero(), JPH::Vec3(gravity.x, gravity.y, gravity.z), deltaTime);
    }
#endif
}

void Water::updateUnderwater(Scene* scene){
    // only a running scene: the editor's Scene view of a stopped scene must not edit its chain
    if (!scene || !Engine::isSceneRunning(scene))
        return;

    // the passes, and whether one is on (the camera was under last frame)
    bool hasPass = false;
    bool wasUnder = false;
    for (const PostProcessPass& pass : scene->getPostProcessPasses()){
        if (isUnderwaterShader(pass.shader)){
            hasPass = true;
            wasUnder = wasUnder || pass.enabled;
        }
    }
    if (!hasPass)
        return;

    // the main camera under the surface of the water it is in
    const Transform* camera = scene->findComponent<Transform>(scene->getCamera());
    const std::vector<WaterSurface> surfaces = getSurfaces(scene);
    const WaterSurface* surface = nullptr;
    float height = 0.0f;
    bool under = false;
    // a camera inside a Water Excluder (a cabin below the waterline) sees no water around it
    if (camera && !waterKeptDry(scene, camera->worldPosition)){
        const Vector3& eye = camera->worldPosition;
        surface = findSurface(surfaces, eye);
        if (surface){
            height = surface->getHeight(eye.x, eye.z, getTime(scene));
            under = eye.y < height + (wasUnder ? UNDERWATER_HYSTERESIS : 0.0f);
        }
    }

    for (unsigned int i = 0; i < (unsigned int)scene->getPostProcessPasses().size(); i++){
        const PostProcessPass& pass = scene->getPostProcessPasses()[i];
        if (!isUnderwaterShader(pass.shader))
            continue;

        if (under){
            auto set = [&](const char* name, const Vector4& value){
                const PostProcessPass& current = scene->getPostProcessPasses()[i];
                if (!ShaderUniforms::has(current.uniforms, name) || ShaderUniforms::get(current.uniforms, name) != value)
                    scene->setPostProcessUniform(i, name, value);
            };
            set("waterLevel", Vector4(height, 0.0f, 0.0f, 0.0f));
            if (ShaderLibraryValues::getParam(pass.uniforms, pass.shader, "followWater").x > 0.5f){
                set("shallowColor", surface->shallowColor);
                set("deepColor", surface->deepColor);
            }
        }
        if (scene->getPostProcessPasses()[i].enabled != under)
            scene->setPostProcessPassEnabled(i, under);
    }
}

void Water::makeRipplesTexture(uint32_t* pixels, int size){
    // a height of sines on whole wave numbers (cycles per tile), so the map tiles
    struct Wave{ float kx, ky, amplitude, phase; };
    static const Wave waves[] = {
        {3, 1, 1.0f, 0.1f}, {-2, 3, 0.8f, 1.3f}, {5, -2, 0.55f, 2.1f}, {1, 6, 0.45f, 0.7f},
        {-7, 2, 0.3f, 2.9f}, {4, 5, 0.3f, 4.0f}, {9, -3, 0.18f, 5.2f}, {-3, -8, 0.16f, 0.4f},
        {11, 4, 0.1f, 3.3f}, {2, -12, 0.09f, 1.9f}
    };

    std::vector<Vector2> slopes((size_t)size * size);
    double squares = 0.0;
    for (int y = 0; y < size; y++){
        for (int x = 0; x < size; x++){
            const float u = (x + 0.5f) / size;
            const float v = (y + 0.5f) / size;
            Vector2 slope(0.0f, 0.0f);
            for (const Wave& wave : waves){
                const float c = std::cos(WATER_TWO_PI * (wave.kx * u + wave.ky * v) + wave.phase) * wave.amplitude * WATER_TWO_PI;
                slope.x += c * wave.kx;
                slope.y += c * wave.ky;
            }
            slopes[(size_t)y * size + x] = slope;
            squares += slope.x * slope.x + slope.y * slope.y;
        }
    }

    // the normal's xy about 0.3 on average
    const float scale = 0.3f / (float)std::sqrt(std::max(squares / ((double)size * size), 1e-12));
    for (size_t i = 0; i < slopes.size(); i++){
        const Vector3 normal = Vector3(-slopes[i].x * scale, -slopes[i].y * scale, 1.0f).normalized();
        pixels[i] = packPixel(normal.x * 0.5f + 0.5f, normal.y * 0.5f + 0.5f, normal.z * 0.5f + 0.5f);
    }
}

void Water::makeNoiseTexture(uint32_t* pixels, int size){
    // four octaves of value noise on lattices of 4 to 32 cells that wrap with the tile
    static const int periods[] = {4, 8, 16, 32};
    static const float weights[] = {0.5f, 0.25f, 0.15f, 0.1f};

    std::vector<float> values((size_t)size * size, 0.0f);
    for (int octave = 0; octave < 4; octave++){
        const int period = periods[octave];
        auto lattice = [&](int x, int y){
            x = ((x % period) + period) % period;
            y = ((y % period) + period) % period;
            return (hash32((uint32_t)(x + y * 131 + octave * 7919) * 0x9E3779B9u) & 0xFFFFFF) / 16777215.0f;
        };
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                const float fx = (x + 0.5f) * period / size;
                const float fy = (y + 0.5f) * period / size;
                const int ix = (int)std::floor(fx);
                const int iy = (int)std::floor(fy);
                float tx = fx - ix;
                float ty = fy - iy;
                // quintic fade
                tx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
                ty = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
                const float a = lattice(ix, iy) + (lattice(ix + 1, iy) - lattice(ix, iy)) * tx;
                const float b = lattice(ix, iy + 1) + (lattice(ix + 1, iy + 1) - lattice(ix, iy + 1)) * tx;
                values[(size_t)y * size + x] += (a + (b - a) * ty) * weights[octave];
            }
        }
    }

    const auto range = std::minmax_element(values.begin(), values.end());
    const float low = *range.first;
    const float span = std::max(*range.second - low, 1e-6f);
    for (size_t i = 0; i < values.size(); i++){
        const float value = (values[i] - low) / span;
        pixels[i] = packPixel(value, value, value);
    }
}
