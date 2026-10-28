// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SplinePresets.h"

#include "Project.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Transform.h"
#include "io/AtomicFile.h"
#include "object/environment/Water.h"
#include "shader/ShaderUniforms.h"
#include "util/Base64.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

using namespace jasperengine;

namespace {

    struct PresetName{
        editor::SplinePreset preset;
        const char* name;
        const char* display;
    };

    const PresetName kPresets[] = {
        {editor::SplinePreset::Empty, "empty", "Spline"},
        {editor::SplinePreset::Road, "road", "Road"},
        {editor::SplinePreset::Path, "path", "Path"},
        {editor::SplinePreset::Wall, "wall", "Wall"},
        {editor::SplinePreset::Fence, "fence", "Fence"},
        {editor::SplinePreset::River, "river", "River"},
        {editor::SplinePreset::Lake, "lake", "Lake"},
        {editor::SplinePreset::Circle, "circle", "Circle"},
        {editor::SplinePreset::Square, "square", "Square"},
        {editor::SplinePreset::Scatter, "scatter", "Scatter"},
        {editor::SplinePreset::Edge2D, "edge2d", "Edge"},
        {editor::SplinePreset::Fill2D, "fill2d", "Filled Shape"},
        {editor::SplinePreset::Circle2D, "circle2d", "Circle"},
    };

    spline::SplineKnot knotAt(float x, float y, float z, float radius){
        spline::SplineKnot knot;
        knot.position = Vector3(x, y, z);
        knot.radius = radius;
        return knot;
    }

    // a ring of n knots of radius r in XZ (3D) or XY (2D)
    std::vector<spline::SplineKnot> ring(int n, float r, bool xy, float knotRadius){
        std::vector<spline::SplineKnot> knots;
        for (int i = 0; i < n; i++){
            const float a = 6.28318530718f * static_cast<float>(i) / static_cast<float>(n);
            // counter-clockwise seen from above (3D) or from the camera (2D)
            knots.push_back(xy ? knotAt(r * std::cos(a), r * std::sin(a), 0.0f, knotRadius)
                               : knotAt(r * std::cos(a), 0.0f, -r * std::sin(a), knotRadius));
        }
        return knots;
    }

    Material litColor(const char* name, float r, float g, float b, float roughness){
        Material material;
        material.name = name;
        material.baseColorFactor = Vector4(r, g, b, 1.0f);
        material.metallicFactor = 0.0f;
        material.roughnessFactor = roughness;
        return material;
    }

    Material waterMaterial(){
        Material material;
        material.name = "Water";
        material.shader = Water::SHADER;
        // FLOW and MESH_DEPTH read the spline mesh's vertex flow and depth (S18 D9): no scene-depth pass
        material.shaderFeatures = {"BUILTIN_TEXTURES", "REFLECTION", "FLOW", "MESH_DEPTH"};
        material.lighting = MaterialLighting::PIXEL;
        return material;
    }

    // a box glTF (positions, normals, indices; one material), pivot at the origin
    std::string boxGltf(const Vector3& lo, const Vector3& hi, const Vector4& color){
        struct V{ float p[3]; float n[3]; };
        std::vector<V> vertices;
        std::vector<uint16_t> indices;
        const Vector3 c[8] = {
            {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
            {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
        // each face counter-clockwise seen from outside (front faces CCW)
        const int faces[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
        const float normals[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        for (int f = 0; f < 6; f++){
            const uint16_t base = static_cast<uint16_t>(vertices.size());
            for (int k = 0; k < 4; k++){
                const Vector3& p = c[faces[f][k]];
                vertices.push_back({{p.x, p.y, p.z}, {normals[f][0], normals[f][1], normals[f][2]}});
            }
            const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
            for (uint16_t q : quad){
                indices.push_back(static_cast<uint16_t>(base + q));
            }
        }
        std::vector<unsigned char> bytes(vertices.size() * sizeof(V) + indices.size() * sizeof(uint16_t));
        std::memcpy(bytes.data(), vertices.data(), vertices.size() * sizeof(V));
        std::memcpy(bytes.data() + vertices.size() * sizeof(V), indices.data(), indices.size() * sizeof(uint16_t));
        const size_t vertexBytes = vertices.size() * sizeof(V);
        const size_t indexBytes = indices.size() * sizeof(uint16_t);
        auto f = [](float v){ return std::to_string(v); };
        std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"JasperEngine spline presets\"},"
            "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":0}]}],"
            "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" + f(color.x) + "," + f(color.y) + "," + f(color.z) + "," +
            f(color.w) + "],\"metallicFactor\":0,\"roughnessFactor\":0.85}}],"
            "\"buffers\":[{\"byteLength\":" + std::to_string(bytes.size()) + ",\"uri\":\"data:application/octet-stream;base64," +
            Base64::encode(bytes.data(), bytes.size()) + "\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(vertexBytes) + ",\"byteStride\":24,\"target\":34962},"
            "{\"buffer\":0,\"byteOffset\":" + std::to_string(vertexBytes) + ",\"byteLength\":" + std::to_string(indexBytes) + ",\"target\":34963}],"
            "\"accessors\":[{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":" + std::to_string(vertices.size()) +
            ",\"type\":\"VEC3\",\"min\":[" + f(lo.x) + "," + f(lo.y) + "," + f(lo.z) + "],\"max\":[" + f(hi.x) + "," + f(hi.y) + "," + f(hi.z) + "]},"
            "{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5126,\"count\":" + std::to_string(vertices.size()) + ",\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5123,\"count\":" + std::to_string(indices.size()) + ",\"type\":\"SCALAR\"}]}";
        return json;
    }

}

bool editor::SplinePresets::fromName(const std::string& name, SplinePreset& preset){
    for (const PresetName& p : kPresets){
        if (name == p.name){
            preset = p.preset;
            return true;
        }
    }
    return false;
}

const char* editor::SplinePresets::name(SplinePreset preset){
    for (const PresetName& p : kPresets){
        if (p.preset == preset) return p.name;
    }
    return "empty";
}

std::vector<std::string> editor::SplinePresets::names(){
    std::vector<std::string> list;
    for (const PresetName& p : kPresets){
        list.push_back(p.name);
    }
    return list;
}

bool editor::SplinePresets::is2D(SplinePreset preset){
    return preset == SplinePreset::Edge2D || preset == SplinePreset::Fill2D || preset == SplinePreset::Circle2D;
}

const char* editor::SplinePresets::displayName(SplinePreset preset){
    for (const PresetName& p : kPresets){
        if (p.preset == preset) return p.display;
    }
    return "Spline";
}

std::string editor::SplinePresets::ensurePiece(Project* project, const char* name){
    const std::string relative = std::string("splines/") + name + ".gltf";
    if (!project){
        return relative;
    }
    const std::filesystem::path file = project->getAssetsPath() / relative;
    std::error_code ec;
    if (std::filesystem::exists(file, ec)){
        return relative;
    }
    std::string json;
    if (std::strcmp(name, "fence_post") == 0){
        json = boxGltf(Vector3(-0.07f, 0.0f, -0.07f), Vector3(0.07f, 1.2f, 0.07f), Vector4(0.42f, 0.30f, 0.18f, 1.0f));
    }else if (std::strcmp(name, "fence_rail") == 0){
        // a span along Z, 2.4 long (the default post pitch), between the posts' heights
        json = boxGltf(Vector3(-0.03f, 0.35f, -1.2f), Vector3(0.03f, 1.0f, 1.2f), Vector4(0.55f, 0.40f, 0.24f, 1.0f));
    }else{
        json = boxGltf(Vector3(-0.2f, 0.0f, -0.2f), Vector3(0.2f, 0.4f, 0.2f), Vector4(0.6f, 0.6f, 0.58f, 1.0f));
    }
    std::filesystem::create_directories(file.parent_path(), ec);
    io::writeFileAtomic(file, json);
    return relative;
}

editor::SplinePresetSetup editor::SplinePresets::make(Project* project, SplinePreset preset, bool scene2D){
    SplinePresetSetup setup;
    SplineComponent& s = setup.spline;
    const bool flat = is2D(preset) || (scene2D && (preset == SplinePreset::Empty || preset == SplinePreset::Circle || preset == SplinePreset::Square));
    if (flat){
        s.spline.setUpAxis(Vector3(0.0f, 0.0f, 1.0f));
    }
    SplineExtrudeComponent& e = setup.extrude;
    SplineInstantiateComponent& i = setup.instantiate;

    switch (preset){
        case SplinePreset::Empty:
            break;
        case SplinePreset::Road:
            s.spline.setKnots({knotAt(-30, 0, 0, 4), knotAt(-10, 0, -4, 4), knotAt(10, 0, 4, 4), knotAt(30, 0, 0, 4)});
            setup.hasExtrude = true;
            e.shape = SplineExtrudeShape::Crown;
            e.conform = SplineConform::TerrainHeight;
            e.verticalOffset = 0.05f;
            e.tilesPerMetreU = 0.125f;
            e.tilesPerMetreV = 0.125f;
            setup.material = litColor("Road", 0.22f, 0.22f, 0.23f, 0.9f);
            // the bed sits under the road's crown (R11)
            s.terrain.flatten = true;
            s.terrain.heightOffset = -e.crownRise;
            s.terrain.extraWidth = 1.0f;
            s.terrain.falloff = 3.0f;
            break;
        case SplinePreset::Path:
            s.spline.setKnots({knotAt(-20, 0, 0, 1.2f), knotAt(-5, 0, 3, 1.2f), knotAt(8, 0, -2, 1.2f), knotAt(20, 0, 1, 1.2f)});
            setup.hasExtrude = true;
            e.shape = SplineExtrudeShape::Bevel;
            e.conform = SplineConform::TerrainHeight;
            e.verticalOffset = 0.03f;
            e.tilesPerMetreU = 0.5f;
            e.tilesPerMetreV = 0.5f;
            setup.material = litColor("Path", 0.45f, 0.36f, 0.25f, 0.95f);
            s.terrain.flatten = true;
            s.terrain.extraWidth = 0.3f;
            s.terrain.falloff = 1.5f;
            break;
        case SplinePreset::Wall:
            s.spline.setType(spline::SplineType::Linear);
            s.spline.setKnots({knotAt(-10, 0, -6, 0.3f), knotAt(10, 0, -6, 0.3f), knotAt(10, 0, 6, 0.3f), knotAt(-10, 0, 6, 0.3f)});
            setup.hasExtrude = true;
            e.shape = SplineExtrudeShape::Rectangle;
            e.corners = SplineCornerStyle::Mitre;
            e.conform = SplineConform::TerrainHeight;
            e.levelCourseV = true;
            e.tilesPerMetreU = 0.5f;
            e.tilesPerMetreV = 0.5f;
            setup.material = litColor("Stone", 0.55f, 0.53f, 0.5f, 0.9f);
            break;
        case SplinePreset::Fence:
            s.spline.setType(spline::SplineType::Linear);
            s.spline.setKnots({knotAt(-12, 0, -4, 0.2f), knotAt(0, 0, -4, 0.2f), knotAt(12, 0, 2, 0.2f)});
            setup.hasInstantiate = true;
            i.mode = SplineInstantiateMode::Fence;
            i.posts.push_back({AssetRef(ensurePiece(project, "fence_post")), 1.0f});
            i.spans.push_back({AssetRef(ensurePiece(project, "fence_rail")), 1.0f});
            i.conform = SplineConform::TerrainHeight;
            i.plantMode = SplinePlantMode::BoundsMin;
            break;
        case SplinePreset::River:
            s.spline.setKnots({knotAt(-40, 0, -10, 6), knotAt(-15, 0, 5, 6), knotAt(15, 0, -5, 6), knotAt(40, 0, 10, 6)});
            setup.hasExtrude = true;
            e.shape = SplineExtrudeShape::WaterBanks;
            e.flow = true;
            e.conform = SplineConform::TerrainHeight;
            e.maxHalfWidth = 14.0f;
            setup.material = waterMaterial();
            setup.castShadows = false;
            // Apply to Terrain carves the channel the water fills
            s.terrain.flatten = true;
            s.terrain.flattenMode = SplineFlattenMode::CutOnly;
            s.terrain.heightOffset = -1.5f;
            s.terrain.extraWidth = 0.0f;
            s.terrain.falloff = 4.0f;
            break;
        case SplinePreset::Lake:
            s.spline.setKnots(ring(8, 15.0f, false, 2.0f));
            s.spline.setClosed(true);
            setup.hasExtrude = true;
            e.shape = SplineExtrudeShape::WaterBanks;
            e.conform = SplineConform::TerrainHeight;
            e.maxHalfWidth = 40.0f;
            setup.material = waterMaterial();
            setup.castShadows = false;
            s.terrain.region = SplineTerrainRegion::Area;
            s.terrain.flatten = true;
            s.terrain.flattenMode = SplineFlattenMode::CutOnly;
            s.terrain.heightOffset = -2.0f;
            s.terrain.falloff = 6.0f;
            break;
        case SplinePreset::Circle:
            s.spline.setKnots(ring(8, 10.0f, flat, 1.0f));
            s.spline.setClosed(true);
            break;
        case SplinePreset::Square:
            s.spline.setType(spline::SplineType::Linear);
            if (flat){
                s.spline.setKnots({knotAt(-5, -5, 0, 1), knotAt(5, -5, 0, 1), knotAt(5, 5, 0, 1), knotAt(-5, 5, 0, 1)});
            }else{
                s.spline.setKnots({knotAt(-5, 0, 5, 1), knotAt(5, 0, 5, 1), knotAt(5, 0, -5, 1), knotAt(-5, 0, -5, 1)});
            }
            s.spline.setClosed(true);
            break;
        case SplinePreset::Scatter:
            s.spline.setKnots({knotAt(-15, 0, 0, 1), knotAt(0, 0, 6, 1), knotAt(15, 0, 0, 1)});
            setup.hasInstantiate = true;
            i.mode = SplineInstantiateMode::Spacing;
            i.items.push_back({AssetRef(ensurePiece(project, "scatter_marker")), 1.0f});
            i.spacing = 2.0f;
            i.yawJitter = 180.0f;
            i.scaleRange = Vector2(0.8f, 1.2f);
            i.conform = SplineConform::TerrainHeight;
            break;
        case SplinePreset::Edge2D:
            s.spline.setKnots({knotAt(-8, -2, 0, 0.25f), knotAt(-3, -3, 0, 0.25f), knotAt(3, -2.5f, 0, 0.25f), knotAt(8, -1, 0, 0.25f)});
            setup.hasExtrude = true;
            e.shape = SplineExtrudeShape::Flat;
            e.collision = SplineCollision::Chain2D;
            e.tilesPerMetreU = 1.0f;
            setup.material = litColor("Edge", 0.35f, 0.55f, 0.25f, 1.0f);
            setup.material.lighting = MaterialLighting::NONE;
            setup.castShadows = false;
            break;
        case SplinePreset::Fill2D:
            s.spline.setType(spline::SplineType::Linear);
            // concave: a notch in the top edge
            s.spline.setKnots({knotAt(-4, -3, 0, 1), knotAt(4, -3, 0, 1), knotAt(4, 3, 0, 1), knotAt(0, 0.5f, 0, 1), knotAt(-4, 3, 0, 1)});
            s.spline.setClosed(true);
            setup.hasExtrude = true;
            e.shape = SplineExtrudeShape::Fill;
            e.tilesPerMetreU = 0.25f;
            e.tilesPerMetreV = 0.25f;
            setup.material = litColor("Fill", 0.3f, 0.45f, 0.75f, 1.0f);
            setup.material.lighting = MaterialLighting::NONE;
            setup.castShadows = false;
            break;
        case SplinePreset::Circle2D:
            s.spline.setKnots(ring(8, 4.0f, true, 0.5f));
            s.spline.setClosed(true);
            break;
    }
    return setup;
}

void editor::SplinePresets::addTo(Scene* scene, Entity entity, const SplinePresetSetup& setup){
    if (!scene->findComponent<Transform>(entity)){
        scene->addComponent<Transform>(entity, {});
    }
    scene->addComponent<SplineComponent>(entity, setup.spline);
    if (setup.hasExtrude){
        MeshComponent mesh;
        mesh.numSubmeshes = 1;
        mesh.submeshes[0].material = setup.material;
        mesh.castShadows = setup.castShadows;
        mesh.receiveShadows = setup.receiveShadows;
        scene->addComponent<MeshComponent>(entity, mesh);
        scene->addComponent<SplineExtrudeComponent>(entity, setup.extrude);
    }
    if (setup.hasInstantiate){
        scene->addComponent<SplineInstantiateComponent>(entity, setup.instantiate);
    }
}
