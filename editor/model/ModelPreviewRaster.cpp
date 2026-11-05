// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelPreviewRaster.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using Json = GltfDocument::Json;
    using Mat = std::array<double, 16>;     // column-major

    Mat identity(){ return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

    Mat multiply(const Mat& a, const Mat& b){
        Mat out{};
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++){
                double sum = 0.0;
                for (int k = 0; k < 4; k++) sum += a[k * 4 + r] * b[c * 4 + k];
                out[c * 4 + r] = sum;
            }
        return out;
    }

    Mat localOf(const Json& node){
        if (node.contains("matrix") && node["matrix"].is_array() && node["matrix"].size() == 16){
            Mat m{};
            for (int i = 0; i < 16; i++) m[i] = node["matrix"][i].get<double>();
            return m;
        }
        double t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
        if (node.contains("translation") && node["translation"].size() == 3) for (int i = 0; i < 3; i++) t[i] = node["translation"][i].get<double>();
        if (node.contains("rotation") && node["rotation"].size() == 4) for (int i = 0; i < 4; i++) q[i] = node["rotation"][i].get<double>();
        if (node.contains("scale") && node["scale"].size() == 3) for (int i = 0; i < 3; i++) s[i] = node["scale"][i].get<double>();
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        Mat m = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
                 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
                 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
                 t[0], t[1], t[2], 1};
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++) m[c * 4 + r] *= s[c];
        return m;
    }

}

bool editor::loadPreviewGeometry(const GltfDocument& doc, ModelPreviewGeometry& out, std::string& error){
    out = ModelPreviewGeometry();
    const Json& root = doc.json();
    const Json* nodes = GltfDocument::arrayOf(root, "nodes");
    const Json* meshes = GltfDocument::arrayOf(root, "meshes");
    if (!nodes || !meshes)
        return true;
    std::vector<size_t> roots;
    size_t sceneIndex = 0;
    const Json* scenes = GltfDocument::arrayOf(root, "scenes");
    if (scenes && !scenes->empty()){
        if (!GltfDocument::getIndex(root, "scene", sceneIndex) || sceneIndex >= scenes->size()) sceneIndex = 0;
        for (const Json& node : (*scenes)[sceneIndex].value("nodes", Json::array()))
            if (node.is_number_unsigned() || node.is_number_integer()) roots.push_back(node.get<size_t>());
    }else{
        std::vector<bool> child(nodes->size(), false);
        for (const Json& node : *nodes)
            for (const Json& c : node.value("children", Json::array()))
                if (c.is_number() && c.get<size_t>() < child.size()) child[c.get<size_t>()] = true;
        for (size_t n = 0; n < nodes->size(); n++)
            if (!child[n]) roots.push_back(n);
    }
    std::vector<bool> visited(nodes->size(), false);
    std::function<bool(size_t, const Mat&)> walk = [&](size_t index, const Mat& parent) -> bool {
        if (index >= nodes->size() || visited[index])
            return true;
        visited[index] = true;
        const Json& node = (*nodes)[index];
        const Mat world = multiply(parent, localOf(node));
        size_t mesh = 0;
        if (GltfDocument::getIndex(node, "mesh", mesh) && mesh < meshes->size()){
            for (const Json& primitive : (*meshes)[mesh].value("primitives", Json::array())){
                if (primitive.value("mode", 4) != 4 || !primitive.contains("attributes") || !primitive["attributes"].contains("POSITION"))
                    continue;
                std::vector<float> positions;
                int components = 0;
                if (!doc.readAccessorFloats(primitive["attributes"]["POSITION"].get<size_t>(), positions, components, error) || components != 3)
                    return false;
                std::vector<uint32_t> list;
                if (primitive.contains("indices")){
                    if (!doc.readIndices(primitive["indices"].get<size_t>(), list, error))
                        return false;
                }else{
                    list.resize(positions.size() / 3);
                    for (size_t v = 0; v < list.size(); v++) list[v] = (uint32_t)v;
                }
                const uint32_t base = (uint32_t)(out.positions.size() / 3);
                for (size_t v = 0; v + 2 < positions.size(); v += 3){
                    const double p[3] = {positions[v], positions[v + 1], positions[v + 2]};
                    for (int r = 0; r < 3; r++)
                        out.positions.push_back((float)(world[r] * p[0] + world[4 + r] * p[1] + world[8 + r] * p[2] + world[12 + r]));
                }
                std::vector<std::vector<uint32_t>> levels = {list};
                const Json lods = primitive.value("extensions", Json::object()).value("JE_mesh_lods", Json::object()).value("levels", Json::array());
                for (const Json& level : lods){
                    std::vector<uint32_t> indices;
                    if (level.contains("indices") && doc.readIndices(level["indices"].get<size_t>(), indices, error))
                        levels.push_back(std::move(indices));
                    error.clear();
                }
                out.levelCount = std::max(out.levelCount, (int)std::min<size_t>(levels.size(), 4));
                for (size_t l = 0; l < 4; l++){
                    const std::vector<uint32_t>& use = levels[std::min(l, levels.size() - 1)];
                    for (size_t i = 0; i + 2 < use.size(); i += 3){
                        if (use[i] >= positions.size() / 3 || use[i + 1] >= positions.size() / 3 || use[i + 2] >= positions.size() / 3)
                            continue;
                        out.levels[l].insert(out.levels[l].end(), {base + use[i], base + use[i + 1], base + use[i + 2]});
                    }
                }
            }
        }
        for (const Json& c : node.value("children", Json::array()))
            if (c.is_number() && !walk(c.get<size_t>(), world))
                return false;
        return true;
    };
    for (size_t r : roots)
        if (!walk(r, identity()))
            return false;
    if (out.positions.empty())
        return true;
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float hi[3] = {-lo[0], -lo[1], -lo[2]};
    for (size_t i = 0; i + 2 < out.positions.size(); i += 3)
        for (int c = 0; c < 3; c++){
            lo[c] = std::min(lo[c], out.positions[i + (size_t)c]);
            hi[c] = std::max(hi[c], out.positions[i + (size_t)c]);
        }
    for (int c = 0; c < 3; c++) out.center[c] = (lo[c] + hi[c]) * 0.5f;
    out.radius = std::max(1e-6f, 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])));
    return true;
}

std::vector<uint8_t> editor::rasterizePreview(const ModelPreviewGeometry& g, int width, int height, float yaw, float pitch, bool wireframe, int level){
    std::vector<uint8_t> rgba((size_t)width * height * 4);
    // the background: a vertical gradient
    for (int y = 0; y < height; y++){
        const uint8_t shade = (uint8_t)(52 + 28 * y / std::max(1, height - 1));
        for (int x = 0; x < width; x++){
            uint8_t* p = &rgba[((size_t)y * width + x) * 4];
            p[0] = p[1] = shade;
            p[2] = (uint8_t)(shade + 8);
            p[3] = 255;
        }
    }
    const std::vector<uint32_t>& triangles = g.levels[(size_t)std::clamp(level, 0, 3)];
    if (g.positions.empty() || triangles.empty())
        return rgba;
    // orthographic view: yaw about y, then pitch about x, fitted to the bounding sphere
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float scale = 0.92f * (float)std::min(width, height) / (2.0f * g.radius);
    const size_t vertexCount = g.positions.size() / 3;
    std::vector<float> screen(vertexCount * 3);
    for (size_t v = 0; v < vertexCount; v++){
        const float x = g.positions[v * 3] - g.center[0], y = g.positions[v * 3 + 1] - g.center[1], z = g.positions[v * 3 + 2] - g.center[2];
        const float x1 = cy * x + sy * z, z1 = -sy * x + cy * z;
        const float y2 = cp * y - sp * z1, z2 = sp * y + cp * z1;
        screen[v * 3] = width * 0.5f + x1 * scale;
        screen[v * 3 + 1] = height * 0.5f - y2 * scale;
        screen[v * 3 + 2] = z2;     // toward the viewer is +z
    }
    std::vector<float> depth((size_t)width * height, -std::numeric_limits<float>::max());
    auto shadeOf = [&](size_t a, size_t b, size_t c){
        // the face normal in view space (two-sided: a model may show its back faces)
        const float ux = screen[b * 3] - screen[a * 3], uy = screen[b * 3 + 1] - screen[a * 3 + 1], uz = screen[b * 3 + 2] - screen[a * 3 + 2];
        const float vx = screen[c * 3] - screen[a * 3], vy = screen[c * 3 + 1] - screen[a * 3 + 1], vz = screen[c * 3 + 2] - screen[a * 3 + 2];
        const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length <= 0.0f) return 0.5f;
        // light from the viewer, a little from above (screen y grows down)
        return 0.25f + 0.75f * std::fabs((nz * 0.85f - ny * 0.45f) / length);
    };
    for (size_t t = 0; t + 2 < triangles.size(); t += 3){
        const size_t a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
        const float* pa = &screen[a * 3];
        const float* pb = &screen[b * 3];
        const float* pc = &screen[c * 3];
        const float area = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
        if (std::fabs(area) < 1e-12f) continue;
        const int x0 = std::max(0, (int)std::floor(std::min({pa[0], pb[0], pc[0]})));
        const int x1 = std::min(width - 1, (int)std::ceil(std::max({pa[0], pb[0], pc[0]})));
        const int y0 = std::max(0, (int)std::floor(std::min({pa[1], pb[1], pc[1]})));
        const int y1 = std::min(height - 1, (int)std::ceil(std::max({pa[1], pb[1], pc[1]})));
        const float light = shadeOf(a, b, c);
        const uint8_t r = (uint8_t)(200 * light), gr = (uint8_t)(196 * light), bl = (uint8_t)(186 * light);
        for (int y = y0; y <= y1; y++){
            for (int x = x0; x <= x1; x++){
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((pb[0] - px) * (pc[1] - py) - (pb[1] - py) * (pc[0] - px)) / area;
                const float w1 = ((pc[0] - px) * (pa[1] - py) - (pc[1] - py) * (pa[0] - px)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float z = w0 * pa[2] + w1 * pb[2] + w2 * pc[2];
                float& d = depth[(size_t)y * width + x];
                if (z <= d) continue;
                d = z;
                uint8_t* p = &rgba[((size_t)y * width + x) * 4];
                p[0] = wireframe ? (uint8_t)(r * 0.45f) : r;
                p[1] = wireframe ? (uint8_t)(gr * 0.45f) : gr;
                p[2] = wireframe ? (uint8_t)(bl * 0.45f) : bl;
            }
        }
    }
    if (!wireframe)
        return rgba;
    // the edges in front: drawn where they are not behind a nearer surface
    auto line = [&](size_t a, size_t b){
        const float* pa = &screen[a * 3];
        const float* pb = &screen[b * 3];
        const int steps = std::max(1, (int)std::ceil(std::max(std::fabs(pb[0] - pa[0]), std::fabs(pb[1] - pa[1]))));
        const float bias = g.radius * 0.01f;
        for (int i = 0; i <= steps; i++){
            const float s = (float)i / steps;
            const int x = (int)(pa[0] + (pb[0] - pa[0]) * s), y = (int)(pa[1] + (pb[1] - pa[1]) * s);
            if (x < 0 || y < 0 || x >= width || y >= height) continue;
            const float z = pa[2] + (pb[2] - pa[2]) * s;
            if (z + bias < depth[(size_t)y * width + x]) continue;
            uint8_t* p = &rgba[((size_t)y * width + x) * 4];
            p[0] = 120;
            p[1] = 210;
            p[2] = 255;
        }
    };
    for (size_t t = 0; t + 2 < triangles.size(); t += 3){
        line(triangles[t], triangles[t + 1]);
        line(triangles[t + 1], triangles[t + 2]);
        line(triangles[t + 2], triangles[t]);
    }
    return rgba;
}
