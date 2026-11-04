// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a glTF model draws, independent of how a tool laid it out (docs/specs/S53-model-import-cook.md
// AC1): the triangles of its default scene in world space, with their normals, first UV set and
// colours, read from a parsed (normalised) tinygltf model. gltfpack and gltf-transform reorder
// vertices and triangles, merge nodes and move transforms into the dequantization of positions,
// so two files draw the same when their soups match triangle for triangle within a tolerance.
// Shared by the core and the editor tests (the Draco and WebP fixtures).

#ifndef JASPER_TEST_GLTF_TRIANGLE_SOUP_H
#define JASPER_TEST_GLTF_TRIANGLE_SOUP_H

#include "tiny_gltf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace jasperengine { namespace test {

    inline std::string s41Fixture(const std::string& name){
        return std::string(JASPER_TEST_REPO_DIR "/engine/tests/core/fixtures/gltf/s41/") + name;
    }

    struct SoupCorner {
        double position[3] = {0, 0, 0};
        double normal[3] = {0, 0, 0};
        double uv[2] = {0, 0};
        double color[4] = {1, 1, 1, 1};
    };

    struct SoupTriangle {
        std::array<SoupCorner, 3> corners;
        bool hasNormal = false;
        bool hasUv = false;
        bool hasColor = false;
        int material = -1;
    };

    struct SoupTolerance {
        double position = 2e-3;   // metres
        double normalDot = 0.98;  // cosine
        double uv = 2e-3;
        double color = 2.5 / 255.0;
    };

    // an accessor's values as glTF reads them (normalized integers to [0, 1] or [-1, 1])
    inline bool soupRead(const tinygltf::Model& model, int index, std::vector<double>& out, int& components){
        if (index < 0 || index >= (int)model.accessors.size())
            return false;
        const tinygltf::Accessor& accessor = model.accessors[index];
        components = tinygltf::GetNumComponentsInType((uint32_t)accessor.type);
        const int size = tinygltf::GetComponentSizeInBytes((uint32_t)accessor.componentType);
        if (components <= 0 || size <= 0 || accessor.bufferView < 0 || accessor.bufferView >= (int)model.bufferViews.size())
            return false;
        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        const std::vector<unsigned char>& data = model.buffers[view.buffer].data;
        const size_t stride = view.byteStride ? view.byteStride : (size_t)components * size;
        const size_t base = view.byteOffset + accessor.byteOffset;
        if (accessor.count && base + (accessor.count - 1) * stride + (size_t)components * size > data.size())
            return false;
        out.resize(accessor.count * components);
        for (size_t e = 0; e < accessor.count; e++){
            for (int c = 0; c < components; c++){
                const unsigned char* p = data.data() + base + e * stride + (size_t)c * size;
                double v = 0;
                switch (accessor.componentType){
                    case TINYGLTF_COMPONENT_TYPE_BYTE: v = accessor.normalized ? std::max((int8_t)p[0] / 127.0, -1.0) : (int8_t)p[0]; break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: v = accessor.normalized ? p[0] / 255.0 : p[0]; break;
                    case TINYGLTF_COMPONENT_TYPE_SHORT: { int16_t s; std::memcpy(&s, p, 2); v = accessor.normalized ? std::max(s / 32767.0, -1.0) : s; break; }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t s; std::memcpy(&s, p, 2); v = accessor.normalized ? s / 65535.0 : s; break; }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: { uint32_t s; std::memcpy(&s, p, 4); v = s; break; }
                    case TINYGLTF_COMPONENT_TYPE_FLOAT: { float f; std::memcpy(&f, p, 4); v = f; break; }
                    default: return false;
                }
                out[e * components + c] = v;
            }
        }
        return true;
    }

    // column-major 4x4
    using SoupMatrix = std::array<double, 16>;

    inline SoupMatrix soupMultiply(const SoupMatrix& a, const SoupMatrix& b){
        SoupMatrix r{};
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++)
                for (int k = 0; k < 4; k++)
                    r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
        return r;
    }

    inline SoupMatrix soupNodeMatrix(const tinygltf::Node& node){
        SoupMatrix m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (node.matrix.size() == 16){
            std::copy(node.matrix.begin(), node.matrix.end(), m.begin());
            return m;
        }
        double x = 0, y = 0, z = 0, w = 1;
        if (node.rotation.size() == 4){ x = node.rotation[0]; y = node.rotation[1]; z = node.rotation[2]; w = node.rotation[3]; }
        const double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                             2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                             2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
        for (int c = 0; c < 3; c++){
            const double s = node.scale.size() == 3 ? node.scale[c] : 1.0;
            for (int row = 0; row < 3; row++)
                m[c * 4 + row] = r[c * 3 + row] * s;
        }
        if (node.translation.size() == 3){
            m[12] = node.translation[0]; m[13] = node.translation[1]; m[14] = node.translation[2];
        }
        return m;
    }

    inline void soupAddPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive, const SoupMatrix& world,
                                 std::vector<SoupTriangle>& out){
        auto attribute = [&](const char* name){
            auto found = primitive.attributes.find(name);
            return found == primitive.attributes.end() ? -1 : found->second;
        };
        std::vector<double> positions, normals, uvs, colors, indices;
        int pc = 0, nc = 0, uc = 0, cc = 0, ic = 0;
        if (!soupRead(model, attribute("POSITION"), positions, pc) || pc != 3)
            return;
        const size_t vertices = positions.size() / 3;
        const bool hasNormal = soupRead(model, attribute("NORMAL"), normals, nc) && nc >= 3 && normals.size() / nc == vertices;
        const bool hasUv = soupRead(model, attribute("TEXCOORD_0"), uvs, uc) && uc == 2 && uvs.size() / 2 == vertices;
        const bool hasColor = soupRead(model, attribute("COLOR_0"), colors, cc) && cc >= 3 && colors.size() / cc == vertices;
        if (primitive.indices >= 0){
            if (!soupRead(model, primitive.indices, indices, ic))
                return;
        }else{
            for (size_t v = 0; v < vertices; v++)
                indices.push_back((double)v);
        }
        // the normal matrix: the linear part's inverse transpose
        const double* m = world.data();
        const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
        const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        const double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                               (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                               (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
        for (size_t t = 0; t + 2 < indices.size(); t += 3){
            SoupTriangle triangle;
            triangle.hasNormal = hasNormal;
            triangle.hasUv = hasUv;
            triangle.hasColor = hasColor;
            triangle.material = primitive.material;
            for (int k = 0; k < 3; k++){
                const size_t v = (size_t)indices[t + k];
                if (v >= vertices)
                    return;
                SoupCorner& corner = triangle.corners[k];
                for (int row = 0; row < 3; row++)
                    corner.position[row] = m[row] * positions[v * 3] + m[4 + row] * positions[v * 3 + 1] + m[8 + row] * positions[v * 3 + 2] + m[12 + row];
                if (hasNormal){
                    double length = 0;
                    for (int row = 0; row < 3; row++){
                        // (inv is row-major: transposed, its column `row`)
                        corner.normal[row] = inv[row] * normals[v * nc] + inv[3 + row] * normals[v * nc + 1] + inv[6 + row] * normals[v * nc + 2];
                        length += corner.normal[row] * corner.normal[row];
                    }
                    length = std::sqrt(length);
                    for (int row = 0; row < 3 && length > 0; row++)
                        corner.normal[row] /= length;
                }
                if (hasUv){
                    corner.uv[0] = uvs[v * 2];
                    corner.uv[1] = uvs[v * 2 + 1];
                }
                if (hasColor){
                    for (int ch = 0; ch < cc; ch++)
                        corner.color[ch] = colors[v * cc + ch];
                }
            }
            out.push_back(triangle);
        }
    }

    // the default scene's triangles (every root of scene 0 without a default scene)
    inline std::vector<SoupTriangle> triangleSoup(const tinygltf::Model& model){
        std::vector<SoupTriangle> out;
        const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
        if (sceneIndex >= (int)model.scenes.size())
            return out;
        std::vector<std::pair<int, SoupMatrix>> stack;
        const SoupMatrix identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (int root : model.scenes[sceneIndex].nodes)
            stack.push_back({root, identity});
        while (!stack.empty()){
            auto [node, parent] = stack.back();
            stack.pop_back();
            if (node < 0 || node >= (int)model.nodes.size())
                continue;
            const SoupMatrix world = soupMultiply(parent, soupNodeMatrix(model.nodes[node]));
            const int mesh = model.nodes[node].mesh;
            if (mesh >= 0 && mesh < (int)model.meshes.size()){
                for (const tinygltf::Primitive& primitive : model.meshes[mesh].primitives)
                    soupAddPrimitive(model, primitive, world, out);
            }
            for (int child : model.nodes[node].children)
                stack.push_back({child, world});
        }
        return out;
    }

    inline double soupArea(const SoupTriangle& t){
        const double* a = t.corners[0].position;
        const double* b = t.corners[1].position;
        const double* c = t.corners[2].position;
        const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        return 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }

    // corner k of `a` against corner (k + rotation) % 3 of `b`: the same winding
    inline bool soupCornersMatch(const SoupTriangle& a, const SoupTriangle& b, int rotation, const SoupTolerance& tolerance){
        for (int k = 0; k < 3; k++){
            const SoupCorner& x = a.corners[k];
            const SoupCorner& y = b.corners[(k + rotation) % 3];
            for (int c = 0; c < 3; c++){
                if (std::fabs(x.position[c] - y.position[c]) > tolerance.position)
                    return false;
            }
            if (a.hasNormal && b.hasNormal){
                const double dot = x.normal[0] * y.normal[0] + x.normal[1] * y.normal[1] + x.normal[2] * y.normal[2];
                if (dot < tolerance.normalDot)
                    return false;
            }
            if (a.hasUv && b.hasUv){
                if (std::fabs(x.uv[0] - y.uv[0]) > tolerance.uv || std::fabs(x.uv[1] - y.uv[1]) > tolerance.uv)
                    return false;
            }
            if (a.hasColor && b.hasColor){
                for (int c = 0; c < 4; c++){
                    if (std::fabs(x.color[c] - y.color[c]) > tolerance.color)
                        return false;
                }
            }
        }
        return true;
    }

    // "" when every triangle of `expected` with an area has its match in `actual` (positions,
    // winding, and the normals, UVs and colours both have) and the counts agree; else what differs
    inline std::string compareSoups(const std::vector<SoupTriangle>& expected, const std::vector<SoupTriangle>& actual,
                                    const SoupTolerance& tolerance = SoupTolerance()){
        constexpr double minArea = 1e-7;
        const double cell = 0.05;
        auto key = [&](const SoupTriangle& t){
            double c[3];
            for (int i = 0; i < 3; i++)
                c[i] = (t.corners[0].position[i] + t.corners[1].position[i] + t.corners[2].position[i]) / 3.0;
            return std::make_tuple((long)std::floor(c[0] / cell), (long)std::floor(c[1] / cell), (long)std::floor(c[2] / cell));
        };
        std::map<std::tuple<long, long, long>, std::vector<size_t>> grid;
        size_t actualCount = 0;
        for (size_t i = 0; i < actual.size(); i++){
            if (soupArea(actual[i]) < minArea)
                continue;
            actualCount++;
            grid[key(actual[i])].push_back(i);
        }
        std::vector<bool> used(actual.size(), false);
        size_t expectedCount = 0;
        size_t unmatched = 0;
        std::ostringstream first;
        for (const SoupTriangle& t : expected){
            if (soupArea(t) < minArea)
                continue;
            expectedCount++;
            const auto [kx, ky, kz] = key(t);
            bool found = false;
            for (long dx = -1; dx <= 1 && !found; dx++)
                for (long dy = -1; dy <= 1 && !found; dy++)
                    for (long dz = -1; dz <= 1 && !found; dz++){
                        auto cellIt = grid.find({kx + dx, ky + dy, kz + dz});
                        if (cellIt == grid.end())
                            continue;
                        for (size_t candidate : cellIt->second){
                            if (used[candidate])
                                continue;
                            for (int rotation = 0; rotation < 3 && !found; rotation++){
                                if (soupCornersMatch(t, actual[candidate], rotation, tolerance)){
                                    used[candidate] = true;
                                    found = true;
                                }
                            }
                            if (found)
                                break;
                        }
                    }
            if (!found && unmatched++ == 0){
                first << "first unmatched triangle at (" << t.corners[0].position[0] << ", " << t.corners[0].position[1] << ", "
                      << t.corners[0].position[2] << ")";
            }
        }
        if (unmatched == 0 && expectedCount == actualCount)
            return std::string();
        std::ostringstream out;
        out << expectedCount << " triangles expected, " << actualCount << " drawn, " << unmatched << " without a match";
        if (unmatched)
            out << "; " << first.str();
        return out.str();
    }

} }

#endif // JASPER_TEST_GLTF_TRIANGLE_SOUP_H
