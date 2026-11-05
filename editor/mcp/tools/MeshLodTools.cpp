// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// capture_model_lods (spec S54 R14, group meshes, contract C5): a contact sheet of a mesh's
// detail levels with their numbers. A model file is read and its levels built (or its baked
// JE_mesh_lods read) and rasterized on the CPU in a job; a GameObject is drawn by the real
// renderer, once per forced level, through the Scene view.

#include "device/QualityResolver.h"
#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "EditorHost.h"
#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "pool/MeshLodPool.h"
#include "render/LodFade.h"
#include "subsystem/RenderSystem.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include "stb_image.h"
#include "stb_image_write.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

constexpr int DEFAULT_TILE = 256;
constexpr int MIN_TILE = 64;
constexpr int MAX_TILE = 512;
// a model file's levels switch for a view of this height at the default threshold (1 px)
constexpr float MODEL_VIEW_HEIGHT = 1080.0f;
constexpr float MODEL_THRESHOLD = 1.0f;

std::string fixed(double value, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    return buffer;
}

// ------------------------------------------------------------------ images

struct Tile {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;
};

// PSNR of a tile against LOD 0's, in dB; null for identical tiles (or other sizes)
Json psnrJson(const Tile& a, const Tile& b) {
    if (a.width != b.width || a.height != b.height || a.rgb.empty()) return nullptr;
    double sum = 0.0;
    for (size_t i = 0; i < a.rgb.size(); i++) {
        const double d = (double)a.rgb[i] - (double)b.rgb[i];
        sum += d * d;
    }
    const double mse = sum / (double)a.rgb.size();
    if (mse <= 0.0) return nullptr;
    return std::round(10.0 * std::log10(255.0 * 255.0 / mse) * 100.0) / 100.0;
}

// the tiles side by side, LOD 0 first, as a PNG
bool contactSheet(const std::vector<Tile>& tiles, std::vector<uint8_t>& png, int& width, int& height) {
    width = 0;
    height = 0;
    for (const Tile& tile : tiles) {
        width += tile.width;
        height = std::max(height, tile.height);
    }
    if (width <= 0 || height <= 0) return false;
    std::vector<uint8_t> sheet((size_t)width * height * 3, 0);
    int x0 = 0;
    for (const Tile& tile : tiles) {
        for (int y = 0; y < tile.height; y++)
            std::memcpy(&sheet[((size_t)y * width + x0) * 3], &tile.rgb[(size_t)y * tile.width * 3], (size_t)tile.width * 3);
        // a one-pixel separator after every tile but the last
        x0 += tile.width;
        if (x0 < width)
            for (int y = 0; y < height; y++) std::memset(&sheet[((size_t)y * width + x0 - 1) * 3], 200, 3);
    }
    png.clear();
    auto append = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<uint8_t>*>(context);
        bytes->insert(bytes->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    };
    return stbi_write_png_to_func(append, &png, width, height, 3, sheet.data(), width * 3) != 0;
}

bool decodeTile(const std::string& base64, Tile& tile) {
    const std::vector<uint8_t> png = Base64::decode(base64);
    int w = 0, h = 0, n = 0;
    unsigned char* pixels = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &n, 3);
    if (!pixels) return false;
    tile.width = w;
    tile.height = h;
    tile.rgb.assign(pixels, pixels + (size_t)w * h * 3);
    stbi_image_free(pixels);
    return true;
}

// save_path inside the project (not an editor-owned file); "" or the error
std::string savePng(Project* project, const std::string& savePath, const std::vector<uint8_t>& png, std::string& savedTo) {
    fs::path target;
    std::string error;
    if (!McpRefs::resolveProjectWritePath(project, savePath, target, error)) return "save_path: " + error;
    if (const std::string owned = McpRefs::protectedPlace(project, target); !owned.empty()) return "save_path: " + owned;
    if (!safewrite::writeFileResolvedBeneath(McpRefs::rootFor(project, target), target, std::string(png.begin(), png.end()), error))
        return "Could not write the PNG to " + FileUtils::pathToGenericUtf8(target) + ": " + error;
    savedTo = FileUtils::pathToGenericUtf8(target);
    return {};
}

// ------------------------------------------------------------------ a model file on the CPU

struct CpuPrimitive {
    std::vector<float> positions;   // the primitive's own (what its levels are built over)
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<float> placed;      // positions through the node's matrix (what is drawn)
    std::vector<uint32_t> indices;
    float color[3] = {0.8f, 0.8f, 0.8f};
    float scale = 1.0f;             // the node matrix's largest axis scale
    std::shared_ptr<MeshLodData> levels;
};

// an accessor as floats (normalized integers mapped), `components` per element
bool readFloats(const tinygltf::Model& model, int index, int components, std::vector<float>& out, std::string& error) {
    if (index < 0 || index >= (int)model.accessors.size()) {
        error = "accessor " + std::to_string(index) + " does not exist";
        return false;
    }
    const tinygltf::Accessor& accessor = model.accessors[(size_t)index];
    if (tinygltf::GetNumComponentsInType((uint32_t)accessor.type) != components || accessor.sparse.isSparse ||
            accessor.bufferView < 0 || accessor.bufferView >= (int)model.bufferViews.size()) {
        error = "accessor " + std::to_string(index) + " is not a plain " + std::to_string(components) + "-component one";
        return false;
    }
    const tinygltf::BufferView& view = model.bufferViews[(size_t)accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= (int)model.buffers.size()) {
        error = "accessor " + std::to_string(index) + " has no buffer (a compressed model: capture a GameObject showing it)";
        return false;
    }
    const int size = tinygltf::GetComponentSizeInBytes((uint32_t)accessor.componentType);
    const size_t element = (size_t)size * components;
    const size_t stride = view.byteStride ? view.byteStride : element;
    const size_t begin = view.byteOffset + accessor.byteOffset;
    const std::vector<unsigned char>& data = model.buffers[(size_t)view.buffer].data;
    if (size <= 0 || accessor.count == 0 || stride < element || begin + (accessor.count - 1) * stride + element > data.size()) {
        error = "accessor " + std::to_string(index) + " reads past its buffer";
        return false;
    }
    out.resize(accessor.count * (size_t)components);
    for (size_t i = 0; i < accessor.count; i++) {
        const unsigned char* at = data.data() + begin + i * stride;
        for (int c = 0; c < components; c++) {
            const unsigned char* p = at + (size_t)c * size;
            float value = 0.0f;
            switch (accessor.componentType) {
                case TINYGLTF_COMPONENT_TYPE_FLOAT: std::memcpy(&value, p, 4); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: value = accessor.normalized ? *p / 255.0f : *p; break;
                case TINYGLTF_COMPONENT_TYPE_BYTE: value = accessor.normalized ? std::max(*(const int8_t*)p / 127.0f, -1.0f) : *(const int8_t*)p; break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); value = accessor.normalized ? v / 65535.0f : v; break; }
                case TINYGLTF_COMPONENT_TYPE_SHORT: { int16_t v; std::memcpy(&v, p, 2); value = accessor.normalized ? std::max(v / 32767.0f, -1.0f) : v; break; }
                default: { uint32_t v; std::memcpy(&v, p, 4); value = (float)v; break; }
            }
            out[i * components + c] = std::isfinite(value) ? value : 0.0f;
        }
    }
    return true;
}

void nodeMatrix(const tinygltf::Node& node, double m[16]) {
    if (node.matrix.size() == 16) {
        for (int i = 0; i < 16; i++) m[i] = node.matrix[(size_t)i];
        return;
    }
    const double t[3] = {node.translation.size() == 3 ? node.translation[0] : 0.0, node.translation.size() == 3 ? node.translation[1] : 0.0,
                         node.translation.size() == 3 ? node.translation[2] : 0.0};
    const double s[3] = {node.scale.size() == 3 ? node.scale[0] : 1.0, node.scale.size() == 3 ? node.scale[1] : 1.0,
                         node.scale.size() == 3 ? node.scale[2] : 1.0};
    double x = 0, y = 0, z = 0, w = 1;
    if (node.rotation.size() == 4) {
        x = node.rotation[0]; y = node.rotation[1]; z = node.rotation[2]; w = node.rotation[3];
    }
    const double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++) m[c * 4 + k] = r[c * 3 + k] * s[c];
        m[c * 4 + 3] = 0.0;
    }
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1.0;
}

void multiply(const double a[16], const double b[16], double out[16]) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            double sum = 0.0;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = sum;
        }
}

// the triangle primitives of a glTF or GLB's default scene, each with its node's matrix
bool readModel(const fs::path& file, std::vector<CpuPrimitive>& primitives, std::string& error) {
    std::string ext = FileUtils::pathToUtf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext != ".gltf" && ext != ".glb") {
        error = "capture_model_lods reads glTF and GLB files; for another model capture a GameObject that shows it (entity)";
        return false;
    }
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    // images are not decoded: the sheet shows base colour factors
    loader.SetImageLoader([](tinygltf::Image*, const int, std::string*, std::string*, int, int, const unsigned char*, int, void*) {
        return true;
    }, nullptr);
    const std::string path = FileUtils::pathToUtf8(file);
    const bool loaded = ext == ".glb" ? loader.LoadBinaryFromFile(&model, &err, &warn, path) : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!loaded) {
        error = "cannot read " + FileUtils::pathToGenericUtf8(file.filename()) + ": " + (err.empty() ? std::string("not a glTF file") : err);
        return false;
    }

    std::vector<int> roots;
    const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
    if (sceneIndex >= 0 && sceneIndex < (int)model.scenes.size()) {
        roots = model.scenes[(size_t)sceneIndex].nodes;
    } else {
        for (size_t i = 0; i < model.nodes.size(); i++) roots.push_back((int)i);
    }
    struct Item { int node; double matrix[16]; };
    std::vector<Item> stack;
    for (int root : roots) {
        Item item{root, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        stack.push_back(item);
    }
    size_t guard = 0;
    while (!stack.empty() && guard++ < 100000) {
        const Item item = stack.back();
        stack.pop_back();
        if (item.node < 0 || item.node >= (int)model.nodes.size()) continue;
        const tinygltf::Node& node = model.nodes[(size_t)item.node];
        double local[16];
        Item next{0, {}};
        nodeMatrix(node, local);
        multiply(item.matrix, local, next.matrix);
        for (int child : node.children) {
            next.node = child;
            stack.push_back(next);
        }
        if (node.mesh < 0 || node.mesh >= (int)model.meshes.size()) continue;
        const double* m = next.matrix;
        float scale = 0.0f;
        for (int c = 0; c < 3; c++)
            scale = std::max(scale, (float)std::sqrt(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]));
        for (const tinygltf::Primitive& primitive : model.meshes[(size_t)node.mesh].primitives) {
            if (primitive.mode != -1 && primitive.mode != TINYGLTF_MODE_TRIANGLES) continue;
            auto position = primitive.attributes.find("POSITION");
            if (position == primitive.attributes.end()) continue;
            CpuPrimitive out;
            out.scale = scale > 0.0f ? scale : 1.0f;
            if (!readFloats(model, position->second, 3, out.positions, error)) return false;
            const size_t vertices = out.positions.size() / 3;
            if (auto normal = primitive.attributes.find("NORMAL"); normal != primitive.attributes.end()) {
                if (!readFloats(model, normal->second, 3, out.normals, error) || out.normals.size() != vertices * 3) out.normals.clear();
            }
            if (auto uv = primitive.attributes.find("TEXCOORD_0"); uv != primitive.attributes.end()) {
                if (!readFloats(model, uv->second, 2, out.uvs, error) || out.uvs.size() != vertices * 2) out.uvs.clear();
            }
            error.clear();
            if (primitive.indices >= 0) {
                std::vector<float> indices;
                if (!readFloats(model, primitive.indices, 1, indices, error)) return false;
                for (float index : indices) {
                    if (!(index >= 0.0f && index < (float)vertices)) {
                        error = "an index is past the primitive's " + std::to_string(vertices) + " vertices";
                        return false;
                    }
                    out.indices.push_back((uint32_t)index);
                }
            } else {
                for (uint32_t i = 0; i < vertices; i++) out.indices.push_back(i);
            }
            out.indices.resize(out.indices.size() / 3 * 3);
            if (primitive.material >= 0 && primitive.material < (int)model.materials.size()) {
                const std::vector<double>& factor = model.materials[(size_t)primitive.material].pbrMetallicRoughness.baseColorFactor;
                for (int c = 0; c < 3 && c < (int)factor.size(); c++) out.color[c] = (float)std::clamp(factor[(size_t)c], 0.0, 1.0);
            }
            out.placed.resize(out.positions.size());
            for (size_t v = 0; v < vertices; v++) {
                const float* p = &out.positions[v * 3];
                for (int r = 0; r < 3; r++)
                    out.placed[v * 3 + r] = (float)(m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r]);
            }
            int recipe = 0;
            std::shared_ptr<MeshLodData> baked = MeshLodPool::readGltfLevels(model, primitive, AttributeDataType::UNSIGNED_INT, vertices, recipe);
            if (baked && recipe == kMeshLodRecipe) out.levels = baked;
            primitives.push_back(std::move(out));
        }
    }
    if (primitives.empty()) {
        error = "the model has no triangle mesh";
        return false;
    }
    return true;
}

std::vector<uint32_t> levelIndices(const CpuPrimitive& primitive, unsigned int level) {
    if (level == 0 || !primitive.levels || primitive.levels->numLevels < 2) return primitive.indices;
    const MeshLodData& data = *primitive.levels;
    const MeshLodLevel& lod = data.levels[std::min(level, data.numLevels - 1)];
    std::vector<uint32_t> out(lod.indexCount);
    for (unsigned int i = 0; i < lod.indexCount; i++) {
        if (data.indexType == AttributeDataType::UNSIGNED_INT) {
            std::memcpy(&out[i], data.indices.data() + (size_t)(lod.indexBase + i) * 4, 4);
        } else {
            uint16_t v;
            std::memcpy(&v, data.indices.data() + (size_t)(lod.indexBase + i) * 2, 2);
            out[i] = v;
        }
    }
    return out;
}

// Lambert with the base colour, flat faces, one light from the camera's upper left; every tile
// framed the same (the model's bounding sphere from three quarters above)
Tile rasterize(const std::vector<CpuPrimitive>& primitives, unsigned int level, int size, const float center[3], float radius) {
    Tile tile;
    tile.width = tile.height = size;
    tile.rgb.assign((size_t)size * size * 3, 0);
    for (size_t i = 0; i < tile.rgb.size(); i += 3) {
        tile.rgb[i] = 38;
        tile.rgb[i + 1] = 40;
        tile.rgb[i + 2] = 46;
    }
    std::vector<float> depth((size_t)size * size, FLT_MAX);
    const float tanHalf = std::tan(15.0f * 3.14159265f / 180.0f);
    const float distance = radius / std::sin(15.0f * 3.14159265f / 180.0f) * 1.05f;
    float forward[3] = {-0.55f, -0.45f, -0.7f};
    const float fl = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (float& c : forward) c /= fl;
    const float eye[3] = {center[0] - forward[0] * distance, center[1] - forward[1] * distance, center[2] - forward[2] * distance};
    float right[3] = {forward[1] * 0.0f - forward[2] * 1.0f, forward[2] * 0.0f - forward[0] * 0.0f, forward[0] * 1.0f - forward[1] * 0.0f};
    const float rl = std::max(std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]), 1e-6f);
    for (float& c : right) c /= rl;
    const float up[3] = {right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
                         right[0] * forward[1] - right[1] * forward[0]};
    float light[3] = {-right[0] + up[0] - forward[0], -right[1] + up[1] - forward[1], -right[2] + up[2] - forward[2]};
    const float ll = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    for (float& c : light) c /= ll;

    for (const CpuPrimitive& primitive : primitives) {
        const std::vector<uint32_t> indices = levelIndices(primitive, level);
        const size_t vertices = primitive.placed.size() / 3;
        std::vector<float> sx(vertices), sy(vertices), sz(vertices);
        for (size_t v = 0; v < vertices; v++) {
            const float d[3] = {primitive.placed[v * 3] - eye[0], primitive.placed[v * 3 + 1] - eye[1], primitive.placed[v * 3 + 2] - eye[2]};
            const float z = std::max(d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2], 1e-4f);
            sx[v] = ((d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (z * tanHalf) * 0.5f + 0.5f) * size;
            sy[v] = (0.5f - (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / (z * tanHalf) * 0.5f) * size;
            sz[v] = z;
        }
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
            const float* pa = &primitive.placed[a * 3];
            const float* pb = &primitive.placed[b * 3];
            const float* pc = &primitive.placed[c * 3];
            const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
            const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl <= 0.0f) continue;
            const float lambert = std::fabs(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / nl;
            const float shade = 0.25f + 0.75f * lambert;
            uint8_t rgb[3];
            for (int k = 0; k < 3; k++) rgb[k] = (uint8_t)std::clamp(std::pow(primitive.color[k] * shade, 1.0f / 2.2f) * 255.0f, 0.0f, 255.0f);

            const float area = (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (sy[b] - sy[a]);
            if (std::fabs(area) < 1e-8f) continue;
            const int x0 = std::max(0, (int)std::floor(std::min({sx[a], sx[b], sx[c]})));
            const int x1 = std::min(size - 1, (int)std::ceil(std::max({sx[a], sx[b], sx[c]})));
            const int y0 = std::max(0, (int)std::floor(std::min({sy[a], sy[b], sy[c]})));
            const int y1 = std::min(size - 1, (int)std::ceil(std::max({sy[a], sy[b], sy[c]})));
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((sx[b] - px) * (sy[c] - py) - (sx[c] - px) * (sy[b] - py)) / area;
                    const float w1 = ((sx[c] - px) * (sy[a] - py) - (sx[a] - px) * (sy[c] - py)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                    // perspective-correct depth: 1/z interpolates linearly on screen
                    const float z = 1.0f / (w0 / sz[a] + w1 / sz[b] + w2 / sz[c]);
                    float& stored = depth[(size_t)y * size + x];
                    if (z >= stored) continue;
                    stored = z;
                    std::memcpy(&tile.rgb[((size_t)y * size + x) * 3], rgb, 3);
                }
            }
        }
    }
    return tile;
}

struct ModelCapture {
    std::string text;
    std::vector<ToolImage> images;
};

// the job's work (off the main thread): read, build or take the baked levels, rasterize
Json captureModel(const fs::path& file, const std::string& shown, int size, std::vector<int> wanted, std::vector<uint8_t>& png, std::string& text,
                  std::string& error) {
    std::vector<CpuPrimitive> primitives;
    if (!readModel(file, primitives, error)) return nullptr;

    unsigned int numLevels = 1;
    bool baked = true;
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (CpuPrimitive& primitive : primitives) {
        if (!primitive.levels) {
            baked = false;
            MeshLodSource source;
            source.positions = primitive.positions;
            source.normals = primitive.normals;
            source.uvs = primitive.uvs;
            source.indices = primitive.indices;
            source.indexType = AttributeDataType::UNSIGNED_INT;
            primitive.levels = MeshLodPool::buildLevels(source, MeshLodRecipe::runtime());
        }
        numLevels = std::max(numLevels, primitive.levels->numLevels);
        for (size_t v = 0; v + 2 < primitive.placed.size(); v += 3)
            for (int k = 0; k < 3; k++) {
                lo[k] = std::min(lo[k], primitive.placed[v + k]);
                hi[k] = std::max(hi[k], primitive.placed[v + k]);
            }
    }
    const float center[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
    const float radius = std::max(0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])), 1e-4f);

    if (wanted.empty())
        for (unsigned int l = 0; l < numLevels; l++) wanted.push_back((int)l);
    std::vector<Tile> tiles;
    Tile reference = rasterize(primitives, 0, size, center, radius);
    Json levels = Json::array();
    for (int level : wanted) {
        if (level >= (int)numLevels) continue;
        size_t triangles = 0;
        float levelError = 0.0f;
        float deviation = 0.0f;
        for (const CpuPrimitive& primitive : primitives) {
            triangles += levelIndices(primitive, (unsigned int)level).size() / 3;
            if (level > 0 && level < (int)primitive.levels->numLevels) {
                levelError = std::max(levelError, primitive.levels->levels[level].error);
                deviation = std::max(deviation, primitive.levels->levels[level].error * primitive.scale);
            }
        }
        Tile tile = rasterize(primitives, (unsigned int)level, size, center, radius);
        Json entry = {{"level", level}, {"triangles", triangles}, {"error", levelError}, {"deviation", deviation},
                      {"switches_below_px", level == 0 ? Json(nullptr) : Json(std::round(lodSwitchHeight(deviation, 1.0f, extent, MODEL_THRESHOLD, 1.0f)))},
                      {"psnr_db", level == 0 ? Json(nullptr) : psnrJson(tile, reference)}};
        levels.push_back(entry);
        tiles.push_back(std::move(tile));
    }
    int width = 0, height = 0;
    if (!contactSheet(tiles, png, width, height)) {
        error = "Encoding the contact sheet failed.";
        return nullptr;
    }
    text = shown + ": " + std::to_string(numLevels) + " level(s)" + (baked ? " (baked by the cook)" : " (built as the runtime would)") + ".";
    for (const Json& entry : levels) {
        text += " LOD " + std::to_string(entry["level"].get<int>()) + ": " + std::to_string(entry["triangles"].get<size_t>()) + " tris";
        if (entry["level"].get<int>() > 0) {
            text += ", error " + fixed(entry["error"].get<float>(), 4);
            if (!entry["switches_below_px"].is_null()) text += ", below " + fixed(entry["switches_below_px"].get<double>(), 0) + " px";
            text += entry["psnr_db"].is_null() ? std::string(", same image") : ", PSNR " + fixed(entry["psnr_db"].get<double>(), 1) + " dB";
        }
        text += ";";
    }
    Json data = {{"path", shown}, {"source", baked ? "baked" : "built"}, {"levels", levels}, {"width", width}, {"height", height},
                 {"units", "model"}, {"view_height_px", MODEL_VIEW_HEIGHT}, {"threshold_px", MODEL_THRESHOLD}};
    if (numLevels < 2)
        data["next"] = "No level reduces this model (too few triangles, or every collapse would deviate past the budget).";
    return data;
}

// ------------------------------------------------------------------ a GameObject through the renderer

struct EntityCapture {
    CallPtr call;
    uint32_t sceneId = 0;
    Entity entity = NULL_ENTITY;
    int forcedBefore = -1;
    int size = DEFAULT_TILE;
    std::string savePath;
    std::vector<int> levels;
    size_t next = 0;
    std::vector<Tile> tiles;
    bool done = false;
};
using EntityCapturePtr = std::shared_ptr<EntityCapture>;

MeshComponent* captureMesh(const EntityCapturePtr& capture, Scene** sceneOut = nullptr) {
    Project* project = capture->call->ctx().project();
    SceneProject* sp = project ? project->getScene(capture->sceneId) : nullptr;
    SceneProject* runtime = sp ? McpRefs::runtimeScene(project, sp) : nullptr;
    if (!runtime || !runtime->scene) return nullptr;
    if (sceneOut) *sceneOut = runtime->scene;
    return runtime->scene->findComponent<MeshComponent>(capture->entity);
}

void finishEntityCapture(const EntityCapturePtr& capture, const std::string& failure) {
    if (capture->done) return;
    capture->done = true;
    Scene* scene = nullptr;
    MeshComponent* mesh = captureMesh(capture, &scene);
    if (mesh) mesh->lodForced = capture->forcedBefore;
    const CallPtr& call = capture->call;
    if (!failure.empty()) {
        call->fail(failure);
        return;
    }
    if (!mesh || !scene) {
        call->fail("The GameObject is gone.");
        return;
    }
    Transform* transform = scene->findComponent<Transform>(capture->entity);
    const float worldScale = transform ? std::max({std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.y), std::fabs(transform->worldScale.z)}) : 1.0f;
    const Vector3 extent = mesh->worldAABB.isNull() ? Vector3(0, 0, 0) : mesh->worldAABB.getSize();
    const float worldHeight = std::max(extent.x, std::max(extent.y, extent.z));
    const float bias = mesh->lodBias * QualityResolver::lodBias();

    Json levels = Json::array();
    std::string text = "GameObject " + std::to_string(capture->entity) + ": " + std::to_string(mesh->numLods) + " level(s).";
    for (size_t i = 0; i < capture->levels.size() && i < capture->tiles.size(); i++) {
        const int level = capture->levels[i];
        size_t triangles = 0;
        for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
            const Submesh& submesh = mesh->submeshes[s];
            const MeshLodData* data = submesh.lodData.get();
            if (level == 0 || !data || data->numLevels < 2) triangles += submesh.vertexCount / 3;
            else triangles += data->levels[std::min((unsigned int)level, data->numLevels - 1)].indexCount / 3;
        }
        const float error = level > 0 ? mesh->lodError[level] : 0.0f;
        Json entry = {{"level", level}, {"triangles", triangles}, {"error", error}, {"deviation", error * worldScale},
                      {"switches_below_px", level == 0 ? Json(nullptr)
                          : Json(std::round(lodSwitchHeight(error, worldScale, worldHeight, scene->getMeshLodThreshold(), bias)))},
                      {"psnr_db", i == 0 && level == 0 ? Json(nullptr) : psnrJson(capture->tiles[i], capture->tiles[0])}};
        text += " LOD " + std::to_string(level) + ": " + std::to_string(triangles) + " tris";
        if (level > 0) {
            text += ", error " + fixed(error, 4);
            if (!entry["switches_below_px"].is_null()) text += ", below " + fixed(entry["switches_below_px"].get<double>(), 0) + " px";
            text += entry["psnr_db"].is_null() ? std::string(", same image") : ", PSNR " + fixed(entry["psnr_db"].get<double>(), 1) + " dB";
        }
        text += ";";
        levels.push_back(entry);
    }
    std::vector<uint8_t> png;
    int width = 0, height = 0;
    if (!contactSheet(capture->tiles, png, width, height)) {
        call->fail("Encoding the contact sheet failed.");
        return;
    }
    Json data = {{"entity", capture->entity}, {"source", "renderer"}, {"levels", levels}, {"width", width}, {"height", height},
                 {"units", "mesh"}, {"threshold_px", scene->getMeshLodThreshold()}, {"bias", bias}};
    if (mesh->numLods < 2) {
        data["next"] = !scene->isMeshLodEnabled() ? "Mesh LOD is off in this scene: set_scene_settings {settings: {mesh_lod_enabled: true}}."
                     : !mesh->lodEnabled ? "This mesh has LOD off (its lodEnabled)."
                     : "This mesh has no levels: too few triangles, a 2D mesh, or a custom shader.";
    } else {
        data["next"] = "Tiles are the Scene view: frame_entity first so the GameObject fills it.";
    }
    if (!capture->savePath.empty()) {
        std::string savedTo;
        const std::string writeError = savePng(call->ctx().project(), capture->savePath, png, savedTo);
        if (!writeError.empty()) {
            call->fail(writeError);
            return;
        }
        data["saved_to"] = savedTo;
    }
    ToolResult result = ToolResult::success(text, data);
    result.images.push_back({Base64::encode(png.data(), png.size()), "image/png"});
    call->reply(std::move(result));
}

void captureNextLevel(const EntityCapturePtr& capture) {
    if (capture->next >= capture->levels.size()) {
        finishEntityCapture(capture, {});
        return;
    }
    MeshComponent* mesh = captureMesh(capture);
    if (!mesh) {
        finishEntityCapture(capture, "The GameObject has no mesh any more.");
        return;
    }
    mesh->lodForced = capture->levels[capture->next];
    w3::CaptureRequest request;
    request.view = "scene";
    request.scene = capture->sceneId;
    request.maxWidth = capture->size;
    w3::captureView(capture->call, request, [capture](w3::CaptureResult shot) {
        if (!shot.ok) {
            finishEntityCapture(capture, shot.error);
            return;
        }
        Tile tile;
        if (!decodeTile(shot.image.data, tile)) {
            finishEntityCapture(capture, "Reading the Scene view image back failed.");
            return;
        }
        capture->tiles.push_back(std::move(tile));
        capture->next++;
        captureNextLevel(capture);
    });
}

bool parseLevels(const Json& args, std::vector<int>& levels, std::string& error) {
    levels.clear();
    if (!args.contains("levels") || args["levels"].is_null()) return true;
    for (const Json& value : args["levels"]) {
        if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() >= MAX_MESH_LODS) {
            error = "levels: integers 0.." + std::to_string(MAX_MESH_LODS - 1);
            return false;
        }
        if (std::find(levels.begin(), levels.end(), value.get<int>()) == levels.end()) levels.push_back(value.get<int>());
    }
    std::sort(levels.begin(), levels.end());
    return true;
}

void captureModelLodsTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    if (!project) {
        call->fail("No project is open.");
        return;
    }
    const bool byPath = args.contains("path") && args["path"].is_string();
    const bool byEntity = args.contains("entity") && !args["entity"].is_null();
    if (byPath == byEntity) {
        call->fail("Pass path (a model file) or entity (a GameObject with a mesh), not both.");
        return;
    }
    std::vector<int> levels;
    std::string error;
    if (!parseLevels(args, levels, error)) {
        call->fail(error);
        return;
    }
    const int size = std::clamp(args.value("size", DEFAULT_TILE), MIN_TILE, MAX_TILE);
    const std::string savePath = args.value("save_path", std::string());

    if (byEntity) {
        SceneProject* sp = w3::resolveSceneViewScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!sp) {
            call->fail(error);
            return;
        }
        const Entity entity = w3::resolveLiveEntity(project, sp, args["entity"], error);
        if (entity == NULL_ENTITY) {
            call->fail(error);
            return;
        }
        auto capture = std::make_shared<EntityCapture>();
        capture->call = call;
        capture->sceneId = sp->id;
        capture->entity = entity;
        capture->size = size;
        capture->savePath = savePath;
        Scene* scene = nullptr;
        MeshComponent* mesh = captureMesh(capture, &scene);
        if (!mesh) {
            call->fail("GameObject " + std::to_string(entity) + " has no Mesh.");
            return;
        }
        capture->forcedBefore = mesh->lodForced;
        // its levels are built first (a scene just loaded may still be simplifying them)
        call->waitUntil([capture]() {
            Scene* s = nullptr;
            captureMesh(capture, &s);
            std::shared_ptr<RenderSystem> render = s ? s->getSystem<RenderSystem>() : nullptr;
            return !render || !render->hasPendingMeshLods();
        }, 20000, [capture, levels](bool) {
            MeshComponent* m = captureMesh(capture);
            if (!m) {
                finishEntityCapture(capture, "The GameObject is gone.");
                return;
            }
            capture->levels = levels;
            if (capture->levels.empty())
                for (unsigned int l = 0; l < std::max(m->numLods, 1u); l++) capture->levels.push_back((int)l);
            capture->levels.erase(std::remove_if(capture->levels.begin(), capture->levels.end(),
                [m](int l) { return l >= (int)std::max(m->numLods, 1u); }), capture->levels.end());
            if (capture->levels.empty() || capture->levels[0] != 0)
                capture->levels.insert(capture->levels.begin(), 0);   // PSNR needs LOD 0
            captureNextLevel(capture);
        });
        return;
    }

    fs::path file;
    if (!McpRefs::resolveProjectPath(project, args["path"].get<std::string>(), file, error, true)) {
        call->fail("path: " + error);
        return;
    }
    const std::string shown = args["path"].get<std::string>();
    JobPtr job = ctx.jobs().create("capture_model_lods", "Capture the LODs of " + shown);
    auto state = std::make_shared<ModelCapture>();
    McpContext* context = &ctx;
    job->setProgress(-1.0, "building and drawing the levels");
    jobs::submit([file, shown, size, levels, state, job, context, savePath]() {
        std::vector<uint8_t> png;
        std::string text, failure;
        Json data;
        try {
            data = captureModel(file, shown, size, levels, png, text, failure);
        } catch (const std::exception& e) {
            failure = e.what();
        }
        getEditorHost().enqueueMainThreadTask([state, job, context, data, png, text, failure, savePath]() {
            if (data.is_null()) {
                job->fail(failure.empty() ? std::string("The capture failed.") : failure);
                return;
            }
            Json result = data;
            if (!savePath.empty()) {
                McpWriteScope writeScope;
                std::string savedTo;
                const std::string writeError = savePng(context->project(), savePath, png, savedTo);
                if (!writeError.empty()) {
                    job->fail(writeError, result);
                    return;
                }
                result["saved_to"] = savedTo;
            }
            state->text = text;
            state->images.push_back({Base64::encode(png.data(), png.size()), "image/png"});
            job->succeed(text, result);
        });
    }, {jobs::Lane::Background, jobs::GateClass::Heavy, "capture-model-lods"});

    replyWhenJobDone(call, job, true, 60.0, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(state->text.empty() ? finished.message() : state->text, result);
        reply.images = state->images;
        return reply;
    });
}

} // namespace

void registerMeshLodTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "capture_model_lods";
    tool.title = "Capture Model LODs";
    tool.group = "meshes";
    tool.description =
        "Contact sheet of a mesh's detail levels (LOD 0 first) with each level's triangles, error, the screen height it "
        "switches below and PSNR against LOD 0. path: a glTF/GLB, levels built or baked, drawn on the CPU; entity: drawn "
        "by the renderer in the Scene view, one forced level per tile (frame_entity first).";
    tool.inputSchema = object({
        {"path", assetPath("")},
        {"scene", sceneRef()},
        {"entity", entityRef()},
        {"levels", array(integer("", 0, MAX_MESH_LODS - 1), "Default: all.")},
        {"size", withDefault(integer("Tile width.", MIN_TILE, MAX_TILE), DEFAULT_TILE)},
        {"save_path", string("Also write the PNG here.")}
    });
    tool.annotations.readOnly = true;
    tool.writesFiles = [](const Json& args) { return args.contains("save_path"); };
    tool.allowedWhilePlaying = true;
    tool.handler = captureModelLodsTool;
    registry.addTool(std::move(tool));
}

}
