// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelFixtures.h"

#include "model/GltfDocument.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

GltfBuild::GltfBuild(){
    json = nlohmann::ordered_json::object();
    json["asset"] = {{"version", "2.0"}, {"generator", "jasperengine tests"}};
}

size_t GltfBuild::addView(const void* data, size_t size, int target, size_t stride){
    while (bin.size() % 4 != 0)
        bin.push_back(0);
    const size_t offset = bin.size();
    bin.insert(bin.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    nlohmann::ordered_json view = {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}};
    if (stride) view["byteStride"] = stride;
    if (target) view["target"] = target;
    json["bufferViews"].push_back(view);
    return json["bufferViews"].size() - 1;
}

size_t GltfBuild::addAccessor(size_t view, int componentType, size_t count, const char* type, bool normalized){
    nlohmann::ordered_json accessor = {{"bufferView", view}, {"componentType", componentType}, {"count", count}, {"type", type}};
    if (normalized) accessor["normalized"] = true;
    json["accessors"].push_back(accessor);
    return json["accessors"].size() - 1;
}

size_t GltfBuild::addFloats(const std::vector<float>& values, int components, int target, bool bounds){
    const size_t view = addView(values.data(), values.size() * sizeof(float), target);
    const size_t accessor = addAccessor(view, gltf::FLOAT, values.size() / (size_t)components, gltf::typeName(components));
    if (bounds){
        nlohmann::ordered_json mins = nlohmann::ordered_json::array(), maxs = nlohmann::ordered_json::array();
        for (int c = 0; c < components; c++){
            float lo = values[(size_t)c], hi = values[(size_t)c];
            for (size_t i = (size_t)c; i < values.size(); i += (size_t)components){
                lo = std::min(lo, values[i]);
                hi = std::max(hi, values[i]);
            }
            mins.push_back(lo);
            maxs.push_back(hi);
        }
        json["accessors"][accessor]["min"] = mins;
        json["accessors"][accessor]["max"] = maxs;
    }
    return accessor;
}

size_t GltfBuild::addIndices(const std::vector<uint32_t>& indices, int componentType){
    std::vector<uint8_t> bytes;
    for (uint32_t index : indices){
        if (componentType == gltf::UNSIGNED_SHORT){
            const uint16_t value = (uint16_t)index;
            bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + 2);
        }else{
            bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&index), reinterpret_cast<const uint8_t*>(&index) + 4);
        }
    }
    const size_t view = addView(bytes.data(), bytes.size(), gltf::ELEMENT_ARRAY_BUFFER);
    return addAccessor(view, componentType, indices.size(), "SCALAR");
}

std::vector<uint8_t> GltfBuild::glb() const{
    nlohmann::ordered_json out = json;
    out["buffers"] = nlohmann::ordered_json::array({{{"byteLength", bin.size()}}});
    return GltfDocument::writeGlb(out, bin);
}

void GltfBuild::write(const std::filesystem::path& file) const{
    std::filesystem::create_directories(file.parent_path());
    const std::vector<uint8_t> bytes = glb();
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
}

nlohmann::ordered_json test::addGridPrimitive(GltfBuild& build, const GridOptions& options){
    const int n = options.quads;
    auto height = [&](float x, float z){ return options.bump * std::sin(x * 6.0f) * std::cos(z * 5.0f); };
    std::vector<float> positions, normals, uvs, colors;
    std::vector<uint32_t> indices;
    auto vertex = [&](int i, int j){
        const float x = (float)i / n, z = (float)j / n;
        positions.insert(positions.end(), {x + options.offset, height(x, z), z});
        // the analytic normal of the waves
        const float dx = options.bump * 6.0f * std::cos(x * 6.0f) * std::cos(z * 5.0f);
        const float dz = -options.bump * 5.0f * std::sin(x * 6.0f) * std::sin(z * 5.0f);
        const float length = std::sqrt(dx * dx + 1.0f + dz * dz);
        normals.insert(normals.end(), {-dx / length, 1.0f / length, -dz / length});
        uvs.insert(uvs.end(), {x, z});
        colors.insert(colors.end(), {x, z, 0.5f, 1.0f});
    };
    if (options.welded){
        for (int j = 0; j <= n; j++)
            for (int i = 0; i <= n; i++)
                vertex(i, j);
    }
    std::vector<std::array<uint32_t, 3>> triangles;
    for (int j = 0; j < n; j++){
        for (int i = 0; i < n; i++){
            const int corners[2][3][2] = {{{i, j}, {i, j + 1}, {i + 1, j}}, {{i + 1, j}, {i, j + 1}, {i + 1, j + 1}}};
            for (const auto& triangle : corners){
                std::array<uint32_t, 3> t;
                for (int c = 0; c < 3; c++){
                    if (options.welded){
                        t[(size_t)c] = (uint32_t)(triangle[c][1] * (n + 1) + triangle[c][0]);
                    }else{
                        t[(size_t)c] = (uint32_t)(positions.size() / 3);
                        vertex(triangle[c][0], triangle[c][1]);
                    }
                }
                triangles.push_back(t);
            }
        }
    }
    if (options.shuffled){
        std::mt19937 random((uint32_t)options.seed);
        std::shuffle(triangles.begin(), triangles.end(), random);
    }
    for (const auto& t : triangles)
        indices.insert(indices.end(), t.begin(), t.end());
    nlohmann::ordered_json primitive = {{"attributes", nlohmann::ordered_json::object()}};
    primitive["attributes"]["POSITION"] = build.addFloats(positions, 3, gltf::ARRAY_BUFFER, true);
    if (options.normals) primitive["attributes"]["NORMAL"] = build.addFloats(normals, 3, gltf::ARRAY_BUFFER);
    if (options.uvs) primitive["attributes"]["TEXCOORD_0"] = build.addFloats(uvs, 2, gltf::ARRAY_BUFFER);
    if (options.colors) primitive["attributes"]["COLOR_0"] = build.addFloats(colors, 4, gltf::ARRAY_BUFFER);
    primitive["indices"] = build.addIndices(indices, positions.size() / 3 > 65535 ? gltf::UNSIGNED_INT : gltf::UNSIGNED_SHORT);
    return primitive;
}

std::vector<uint8_t> test::makeTestModel(int quads, bool welded){
    GltfBuild build;
    auto& json = build.json;
    json["materials"] = nlohmann::ordered_json::array({{{"name", "Opaque"}}, {{"name", "Glass"}, {"alphaMode", "BLEND"}}});
    GridOptions grid;
    grid.quads = quads;
    grid.welded = welded;
    nlohmann::ordered_json first = addGridPrimitive(build, grid);
    first["material"] = 0;
    grid.offset = 1.5f;
    grid.seed = 11;
    grid.colors = true;
    nlohmann::ordered_json second = addGridPrimitive(build, grid);
    second["material"] = 1;
    json["meshes"].push_back({{"name", "Body"}, {"primitives", {first, second}}});

    // Prop: a morph target and a skin
    GridOptions small;
    small.quads = std::max(4, quads / 3);
    small.welded = welded;
    small.seed = 3;
    nlohmann::ordered_json prop = addGridPrimitive(build, small);
    const size_t vertices = build.json["accessors"][prop["attributes"]["POSITION"].get<size_t>()]["count"].get<size_t>();
    std::vector<float> deltas(vertices * 3, 0.0f);
    for (size_t v = 0; v < vertices; v++)
        deltas[v * 3 + 1] = 0.05f * (float)(v % 7);
    prop["targets"] = nlohmann::ordered_json::array({{{"POSITION", build.addFloats(deltas, 3, gltf::ARRAY_BUFFER, true)}}});
    std::vector<uint16_t> joints(vertices * 4, 0);
    std::vector<float> weights(vertices * 4, 0.0f);
    for (size_t v = 0; v < vertices; v++)
        weights[v * 4] = 1.0f;
    const size_t jointView = build.addView(joints.data(), joints.size() * 2, gltf::ARRAY_BUFFER);
    prop["attributes"]["JOINTS_0"] = build.addAccessor(jointView, gltf::UNSIGNED_SHORT, vertices, "VEC4");
    prop["attributes"]["WEIGHTS_0"] = build.addFloats(weights, 4, gltf::ARRAY_BUFFER);
    json["meshes"].push_back({{"name", "Prop"}, {"primitives", {prop}}, {"weights", {0.0}}});

    const std::vector<float> inverseBind = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const size_t bind = build.addFloats(inverseBind, 16, 0);
    json["accessors"][bind]["type"] = "MAT4";
    json["accessors"][bind]["count"] = 1;

    json["nodes"] = nlohmann::ordered_json::array({
        {{"name", "Root"}, {"children", {1, 2}}},
        {{"name", "Body"}, {"mesh", 0}},
        {{"name", "Prop"}, {"mesh", 1}, {"skin", 0}, {"translation", {0.0, 0.5, 0.0}}, {"children", {3}}},
        {{"name", "Bone"}}});
    json["skins"] = nlohmann::ordered_json::array({{{"joints", {3}}, {"inverseBindMatrices", bind}}});
    const size_t times = build.addFloats({0.0f, 1.0f}, 1, 0, true);
    const size_t moves = build.addFloats({0, 0, 0, 0, 1, 0}, 3, 0);
    json["animations"] = nlohmann::ordered_json::array({{{"name", "Bob"},
        {"samplers", {{{"input", times}, {"output", moves}, {"interpolation", "LINEAR"}}}},
        {"channels", {{{"sampler", 0}, {"target", {{"node", 2}, {"path", "translation"}}}}}}}});
    json["scenes"] = nlohmann::ordered_json::array({{{"nodes", {0}}}});
    json["scene"] = 0;
    json["extras"] = {{"kept", "by the cook"}};
    return build.glb();
}
