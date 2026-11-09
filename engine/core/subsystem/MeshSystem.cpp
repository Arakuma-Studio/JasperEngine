// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshSystem.h"
#include "terrain/TerrainEditModule.h"
#include "subsystem/FoliageSystem.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"
#include "component/CameraComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/BoneComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/TerrainComponent.h"
#include "pixelart/PixelSnap.h"

#include "Scene.h"
#include "object/TerrainEdit.h"
#include "Engine.h"
#include "System.h"
#include "animation/ModelSubClips.h"
#include "animation/VertexAnimation.h"
#include "buffer/InterleavedBuffer.h"
#include "thread/MainThreadQueue.h"
#include "io/FileData.h"
#include "io/Data.h"
#include "io/ResourcePack.h"
#include "pool/AnimationClipPool.h"
#include "pool/ModelPool.h"
#include "pool/TexturePool.h"
#include "pool/TextureDataPool.h"
#include "sprite/SpriteFrameGeometry.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"
#include "thread/ResourceProgress.h"
#include "io/AssetIOService.h"
#include "subsystem/RenderSystem.h"
#include "model/CookedModels.h"
#include "texture/CookedTextures.h"
#include "texture/Ktx2.h"
#include "util/Angle.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <thread>
#include <utility>
#include "tiny_obj_loader.h"
#include "tiny_gltf.h"
#include "pool/MeshLodPool.h"
#include "stb_image_resize2.h"
#include "meshoptimizer.h"
#include "pool/ObjModelData.h"
#include "subsystem/GltfNormalize.h"
#include "model/ModelCpuData.h"

using namespace jasperengine;

namespace {
    thread_local int g_imageDecodeMaxDimension = 0;

    // an OBJ face corner as loadOBJ lays it out (InterleavedBuffer: position, uv, normal, colour)
    struct ObjCorner {
        Vector3 position;
        Vector2 texcoord;
        Vector3 normal;
        Vector4 color;
    };
    static_assert(sizeof(ObjCorner) == 12 * sizeof(float), "an OBJ corner is twelve packed floats");

    // Welds binary-equal corners (meshopt_generateVertexRemap over every attribute), then orders
    // each submesh's triangles for the vertex cache and overdraw, unless it blends (its triangle
    // order is its draw order), and the vertices by first use. What draws does not change.
    void optimizeObjCorners(std::vector<ObjCorner>& corners, std::vector<std::vector<uint32_t>>& submeshIndices,
                            const std::vector<bool>& keepOrder){
        if (corners.empty())
            return;
        std::vector<unsigned int> remap(corners.size());
        const size_t unique = meshopt_generateVertexRemap(remap.data(), nullptr, corners.size(), corners.data(), corners.size(), sizeof(ObjCorner));
        std::vector<ObjCorner> welded(unique);
        meshopt_remapVertexBuffer(welded.data(), corners.data(), corners.size(), sizeof(ObjCorner), remap.data());
        for (size_t s = 0; s < submeshIndices.size(); s++) {
            std::vector<uint32_t>& indices = submeshIndices[s];
            meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
            if (indices.size() < 3 || indices.size() % 3 != 0 || (s < keepOrder.size() && keepOrder[s]))
                continue;
            meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), unique);
            meshopt_optimizeOverdraw(indices.data(), indices.data(), indices.size(), &welded[0].position.x, unique, sizeof(ObjCorner), 1.05f);
        }
        // first use across the submeshes in their order, as one index buffer
        std::vector<uint32_t> all;
        for (const std::vector<uint32_t>& indices : submeshIndices)
            all.insert(all.end(), indices.begin(), indices.end());
        std::vector<unsigned int> fetch(unique);
        const size_t used = meshopt_optimizeVertexFetchRemap(fetch.data(), all.data(), all.size(), unique);
        corners.assign(used, ObjCorner());
        meshopt_remapVertexBuffer(corners.data(), welded.data(), unique, sizeof(ObjCorner), fetch.data());
        corners.resize(used);
        for (std::vector<uint32_t>& indices : submeshIndices)
            meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), fetch.data());
    }

    InstanceData gltfInstanceData(const GltfInstance& instance){
        InstanceData data;
        data.position = Vector3(instance.translation[0], instance.translation[1], instance.translation[2]);
        data.rotation = Quaternion(instance.rotation[3], instance.rotation[0], instance.rotation[1], instance.rotation[2]);
        data.scale = Vector3(instance.scale[0], instance.scale[1], instance.scale[2]);
        return data;
    }

    Matrix4 gltfInstanceMatrix(const GltfInstance& instance){
        const InstanceData data = gltfInstanceData(instance);
        return Matrix4::translateMatrix(data.position) * data.rotation.getRotationMatrix() * Matrix4::scaleMatrix(data.scale);
    }

    // a direction a node transform can be baked into: float, or a normalized byte or short (the
    // KHR_mesh_quantization normals and tangents GltfNormalize keeps for the GPU)
    bool isBakeableDirection(const tinygltf::Accessor& accessor){
        return accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
               (accessor.normalized && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
                                        accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT));
    }

    // `count` components of an attribute as floats (glTF's normalization of signed integers)
    void readGLTFComponents(const unsigned char* src, int componentType, int count, float* out){
        for (int c = 0; c < count; c++){
            if (componentType == TINYGLTF_COMPONENT_TYPE_BYTE){
                out[c] = std::max((float)(int8_t)src[c] / 127.0f, -1.0f);
            }else if (componentType == TINYGLTF_COMPONENT_TYPE_SHORT){
                int16_t value;
                std::memcpy(&value, src + c * 2, 2);
                out[c] = std::max((float)value / 32767.0f, -1.0f);
            }else{
                std::memcpy(out + c, src + c * 4, 4);
            }
        }
    }

    // loadGLTF's node walk (getLegacyRootTransform repeats it on a loaded model): each node's
    // parent (the last node listing it as a child, -1 for none) and the nodes that own a mesh, in
    // index order
    void collectGLTFNodes(const tinygltf::Model& gltf, std::map<int, int>& nodesParent, std::vector<int>& meshNodes){
        for (size_t i = 0; i < gltf.nodes.size(); i++){
            nodesParent[static_cast<int>(i)] = -1;
        }
        for (size_t i = 0; i < gltf.nodes.size(); i++){
            if (gltf.nodes[i].mesh >= 0){
                meshNodes.push_back(static_cast<int>(i));
            }
            for (int child : gltf.nodes[i].children){
                nodesParent[child] = static_cast<int>(i);
            }
        }
    }

    // the node whose global matrix places the model entity: the default scene's first root, else
    // the first mesh node (-1 without one)
    int gltfRootTransformNode(const tinygltf::Model& gltf, const std::vector<int>& meshNodes){
        if (!gltf.scenes.empty()){
            const int sceneIdx = gltf.defaultScene >= 0 ? gltf.defaultScene : 0;
            if (sceneIdx < (int)gltf.scenes.size() && !gltf.scenes[sceneIdx].nodes.empty()){
                const int node = gltf.scenes[sceneIdx].nodes[0];
                if (node >= 0 && node < (int)gltf.nodes.size()){
                    return node;
                }
            }
        }
        return meshNodes.empty() ? -1 : meshNodes[0];
    }

    // the largest sprite grid a lattice can ask for (LatticeModifierComponent::subdivisions)
    constexpr int SPRITE_MAX_GRID = 64;

    // cells per side the sprite mesh should have: n x n while a lattice deforms it (spec
    // lattice §6.4, LatticeRender::prepare asks for it), else the quad (1)
    int requestedSpriteGrid(const MeshComponent& mesh){
        if (!mesh.lattice.wantsTessellation){
            return 1;
        }
        return std::clamp(mesh.lattice.tessellation, 1, SPRITE_MAX_GRID);
    }

    // cells per side of the sprite mesh as built: (n + 1)^2 vertices, the quad has 4; 0 when
    // it has not been built
    int builtSpriteGrid(const MeshComponent& mesh){
        const unsigned int vertices = mesh.buffer.getCount();
        if (vertices == 0){
            return 0;
        }
        const int side = (int)std::lround(std::sqrt((double)vertices));
        return (side >= 2 && (unsigned int)(side * side) == vertices) ? side - 1 : 0;
    }

    // One tile that made it into a tilemap vertex buffer, in vertex-buffer order.
    struct TilemapAcceptedTile{
        unsigned int vertexBase; // first vertex of the tile quad
        unsigned int submeshId;
        unsigned int chunk;
        AABB aabb;               // local-space quad bounds
    };

    // Emits the index data of an already-built tilemap vertex buffer, grouping the
    // quads into a spatial grid so the renderer can frustum-cull parts of a large map
    // instead of submitting every tile. Vertices keep their tile order: only the index
    // order changes, which makes each (submesh, chunk) pair one contiguous run of
    // indices that a single draw call can address.
    void buildTilemapChunks(TilemapComponent& tilemap, std::vector<TilemapAcceptedTile>& tiles, std::vector<std::vector<uint16_t>>& indexMap){
        const unsigned int numSubmeshes = (unsigned int)indexMap.size();

        tilemap.chunks.clear();
        tilemap.chunkStart.assign(numSubmeshes + 1, 0);

        unsigned int chunksPerAxis = 1;
        if (tiles.size() >= TILEMAP_MIN_CHUNKED_TILES && tilemap.width > 0 && tilemap.height > 0){
            chunksPerAxis = (unsigned int)std::ceil(std::sqrt((double)tiles.size() / TILEMAP_CHUNK_TARGET_TILES));
            chunksPerAxis = std::clamp(chunksPerAxis, 1u, (unsigned int)MAX_TILEMAP_CHUNKS_PER_AXIS);
        }

        // tiles were clamped to non-negative local coords, so the grid spans
        // (0,0)..(width,height) and a tile is placed by the chunk of its center
        const float chunkWidth = (float)tilemap.width / chunksPerAxis;
        const float chunkHeight = (float)tilemap.height / chunksPerAxis;

        for (TilemapAcceptedTile& tile : tiles){
            const Vector3 center = tile.aabb.getCenter();
            unsigned int cx = (chunkWidth > 0) ? (unsigned int)(center.x / chunkWidth) : 0;
            unsigned int cy = (chunkHeight > 0) ? (unsigned int)(center.y / chunkHeight) : 0;
            tile.chunk = (std::min(cy, chunksPerAxis - 1) * chunksPerAxis) + std::min(cx, chunksPerAxis - 1);
        }

        // Groups the tiles by submesh, and by chunk within it, so both are contiguous
        // index runs. Stable, so tiles of the same chunk keep their tile order.
        std::stable_sort(tiles.begin(), tiles.end(), [](const TilemapAcceptedTile& a, const TilemapAcceptedTile& b){
            if (a.submeshId != b.submeshId)
                return a.submeshId < b.submeshId;
            return a.chunk < b.chunk;
        });

        size_t t = 0;
        for (unsigned int s = 0; s < numSubmeshes; s++){
            tilemap.chunkStart[s] = (unsigned int)tilemap.chunks.size();

            // tiles of a submesh are contiguous once sorted, so a submesh without
            // tiles just leaves indexMap[s] empty and gets no chunk
            while (t < tiles.size() && tiles[t].submeshId == s){
                const unsigned int chunk = tiles[t].chunk;

                TilemapChunk range;
                range.offset = (unsigned int)indexMap[s].size();

                while (t < tiles.size() && tiles[t].submeshId == s && tiles[t].chunk == chunk){
                    const uint16_t base = (uint16_t)tiles[t].vertexBase;
                    indexMap[s].push_back(base + 0);
                    indexMap[s].push_back(base + 1);
                    indexMap[s].push_back(base + 2);
                    indexMap[s].push_back(base + 0);
                    indexMap[s].push_back(base + 2);
                    indexMap[s].push_back(base + 3);

                    range.aabb.merge(tiles[t].aabb);
                    t++;
                }

                range.count = (unsigned int)indexMap[s].size() - range.offset;
                tilemap.chunks.push_back(range);
            }
        }
        tilemap.chunkStart[numSubmeshes] = (unsigned int)tilemap.chunks.size();
    }
}

struct MeshSystem::AsyncModelLoadResult {
    bool success = false;
    std::string warn;
    std::string err;
    std::shared_ptr<tinygltf::Model> gltfModel;
    std::shared_ptr<ObjModelData> objModel;
    // Texture pixels copied on the worker thread (id -> pooled face array), handed to
    // TextureDataPool on the main thread so the heavy memcpy stays off it.
    std::vector<std::pair<std::string, std::shared_ptr<std::array<TextureData,6>>>> prebuiltTextures;
};

std::mutex& MeshSystem::getAsyncModelMutex(){
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

MeshSystem::async_model_loads_t& MeshSystem::getPendingModelLoads(){
    static async_model_loads_t* loads = new async_model_loads_t();
    return *loads;
}

namespace {
    // `path` relative to `root` when it is inside it; empty otherwise
    std::string relativeInside(const std::filesystem::path& path, const std::string& root){
        if (root.empty())
            return std::string();
        const std::filesystem::path relative = path.lexically_normal().lexically_relative(std::filesystem::path(root).lexically_normal());
        if (relative.empty() || relative == "." || *relative.begin() == "..")
            return std::string();
        return relative.generic_string();
    }
}

std::string MeshSystem::getModelFilenameKey(const std::string& filename){
    // FileData resolves project-relative and asset:// paths exactly as the file
    // loader does. This keeps the model/texture pools from treating the same
    // file as separate absolute and relative resources.
    return std::filesystem::path(FileData::getSystemPath(filename)).lexically_normal().generic_string();
}

std::string MeshSystem::getModelTextureKey(const std::string& filename){
    const std::string key = getModelFilenameKey(filename);
    const std::string relative = relativeInside(std::filesystem::path(key), System::instance().getAssetPath());
    return relative.empty() ? key : relative;
}

std::string MeshSystem::migrateModelTextureId(const std::string& id, const std::string& assetsRoot){
    const size_t bar = id.find('|');
    if (bar == std::string::npos || assetsRoot.empty())
        return id;
    const std::filesystem::path model(id.substr(0, bar));
    const bool windowsAbsolute = model.native().size() > 1 && model.native()[1] == ':';
    if (!model.is_absolute() && !windowsAbsolute && id.compare(0, 1, "/") != 0)
        return id;
    const std::string rest = id.substr(bar);
    const std::string inside = relativeInside(model, assetsRoot);
    if (!inside.empty())
        return inside + rest;
    // moved: the longest tail of the old path that names a file of this assets directory
    std::vector<std::string> parts;
    for (const auto& part : model.lexically_normal().relative_path())
        parts.push_back(part.generic_string());
    std::error_code ec;
    for (size_t first = 1; first < parts.size(); first++){
        std::filesystem::path tail;
        for (size_t i = first; i < parts.size(); i++)
            tail /= parts[i];
        if (std::filesystem::is_regular_file(std::filesystem::path(assetsRoot) / tail, ec))
            return tail.generic_string() + rest;
    }
    return id;
}

// An external image of a glTF the cooked textures have (S14 R9): its asset path ("textures/a.png"),
// which the texture pool loads cooked; empty for embedded images and the ones not cooked. An
// export ships such an image cooked only, so the model loader finds no file to read.
static std::string gltfCookedImagePath(const std::string& modelFilename, const tinygltf::Image& image){
    if (image.uri.empty() || tinygltf::IsDataURI(image.uri))
        return std::string();
    std::string uri;
    if (!tinygltf::URIDecode(image.uri, &uri, nullptr) || uri.empty())
        return std::string();
    const size_t slash = modelFilename.find_last_of("/\\");
    const std::string joined = (slash == std::string::npos ? std::string() : modelFilename.substr(0, slash + 1)) + uri;
    const std::string path = CookedTextures::normalize(joined);
    CookedTextureEntry entry;
    return !path.empty() && CookedTextures::find(path, entry) ? path : std::string();
}

// tinygltf's warnings less the ones about external images that ship cooked only (not found,
// not loaded): those load from their cooked file; and less stb's "Unknown image format" for the
// KTX2 and WebP images decodeGLTFImage decodes itself (it reports their own failures)
static std::string withoutCookedImageWarnings(const std::string& warn, const tinygltf::Model& model, const std::string& modelFilename){
    std::vector<std::string> cooked;
    std::vector<std::string> decodedLater;
    for (size_t i = 0; i < model.images.size(); i++){
        const tinygltf::Image& image = model.images[i];
        std::string uri;
        if (image.image.empty() && !gltfCookedImagePath(modelFilename, image).empty() && tinygltf::URIDecode(image.uri, &uri, nullptr))
            cooked.push_back(uri);
        const std::span<const uint8_t> bytes(image.image.data(), image.image.size());
        if (GltfNormalize::isWebp(bytes.data(), bytes.size()) || Ktx2::isKtx2(bytes))
            decodedLater.push_back("Unknown image format. STB cannot decode image header for image[" + std::to_string(i) + "]");
    }
    if (cooked.empty() && decodedLater.empty())
        return warn;
    std::string out;
    std::istringstream lines(warn);
    std::string line;
    while (std::getline(lines, line)){
        const bool aboutCooked = std::any_of(cooked.begin(), cooked.end(), [&line](const std::string& uri){
            const bool notFound = line.rfind("File not found", 0) == 0 && line.size() >= uri.size() &&
                                  line.compare(line.size() - uri.size(), uri.size(), uri) == 0;
            const bool notLoaded = line.rfind("Failed to load external 'uri' for image", 0) == 0 &&
                                   line.find("\"" + uri + "\"") != std::string::npos;
            return notFound || notLoaded;
        });
        const bool aboutDecoded = std::any_of(decodedLater.begin(), decodedLater.end(), [&line](const std::string& prefix){
            return line.rfind(prefix, 0) == 0;
        });
        if (!aboutCooked && !aboutDecoded)
            out += line + "\n";
    }
    return out;
}

// a binary glTF by its bytes ("glTF"): a cooked .glb stands in for a .gltf source too
static bool isGlbData(const unsigned char* data, size_t size){
    return size >= 4 && data[0] == 'g' && data[1] == 'l' && data[2] == 'T' && data[3] == 'F';
}

// An FBX model loads from the .glb the editor imported it into (getImportedModelPath)
static bool isFbxModelFile(const std::string& filename){
    std::string ext = FileData::getFilePathExtension(filename);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return ext == "fbx";
}

std::string MeshSystem::getImportedModelPath(const std::string& filename){
    CookedModelEntry cooked;
    if (CookedModels::find(filename, cooked)){
        return cooked.file;
    }
    return isFbxModelFile(filename) ? getFbxImportPath(filename) : filename;
}

std::string MeshSystem::getFbxImportPath(const std::string& filename){
    const std::string importPath = System::instance().getImportPath();
    if (importPath.empty()){
        return filename + ".glb";
    }
    // The editor's cache mirrors the assets directory
    const std::filesystem::path source = std::filesystem::path(FileData::getSystemPath(filename)).lexically_normal();
    const std::filesystem::path assets = std::filesystem::path(System::instance().getAssetPath()).lexically_normal();
    return (std::filesystem::path(importPath) / source.lexically_relative(assets)).generic_string() + ".glb";
}

std::recursive_mutex& MeshSystem::getPooledGLTFMutex(){
    static std::recursive_mutex* mutex = new std::recursive_mutex();
    return *mutex;
}

// Name of a converted or baked copy of an accessor's data appended to a (pooled) glTF model:
// the source data and the bake matrix, so a later load finds the copy instead of adding one
static const char* const GLTF_SYNTH_PREFIX = "jasperengine_synth|";

static std::string gltfSynthKey(const char* kind, const tinygltf::Accessor& accessor, const Matrix4* matrix = nullptr){
    std::string key = std::string(GLTF_SYNTH_PREFIX) + kind + "|" + std::to_string(accessor.bufferView) + "," +
        std::to_string(accessor.byteOffset) + "," + std::to_string(accessor.count) + "," +
        std::to_string(accessor.componentType) + "," + std::to_string(accessor.type);
    if (matrix) {
        char hex[9];
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                uint32_t bits;
                std::memcpy(&bits, &(*matrix)[c][r], sizeof(bits));
                std::snprintf(hex, sizeof(hex), "%08x", bits);
                key += (c == 0 && r == 0 ? "|" : ",");
                key += hex;
            }
        }
    }
    return key;
}

// Key texture storage by image source and sampler, not glTF texture index: glTF files
// often point several texture entries at one image, which keying by index duplicated.
static std::string gltfTextureDedupKey(const std::string& modelKey, const tinygltf::Model& model, int textureIndex){
    int source = -1;
    int sampler = -1;
    if (textureIndex >= 0 && static_cast<size_t>(textureIndex) < model.textures.size()){
        source = model.textures[textureIndex].source;
        sampler = model.textures[textureIndex].sampler;
    }
    return modelKey + "|gltf-image|" + std::to_string(source) + "|s" + std::to_string(sampler);
}

MeshSystem::MeshSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<MeshComponent>());
    // the types onComponentAdded/onComponentRemoved handle
    declareHookType(scene->getComponentId<ModelComponent>());
    declareHookType(scene->getComponentId<TerrainComponent>());
    declareHookType(scene->getComponentId<InstancedMeshComponent>());
}

// the scenes that loaded a model in the background, for JASPER_MESH_LOAD_HOLD_FILE
static const std::string& meshLoadHoldFile(){
    static const std::string file = [](){
        const char* value = std::getenv("JASPER_MESH_LOAD_HOLD_FILE");
        return std::string(value ? value : "");
    }();
    return file;
}

static std::mutex& getMeshLoadHoldMutex(){
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

static std::set<const Scene*>& getMeshLoadHoldScenes(){
    static std::set<const Scene*>* scenes = new std::set<const Scene*>();
    return *scenes;
}

bool MeshSystem::isMeshLoadHeld(const Scene* scene){
    if (meshLoadHoldFile().empty())
        return false;
    {
        std::lock_guard<std::mutex> lock(getMeshLoadHoldMutex());
        if (!getMeshLoadHoldScenes().count(scene))
            return false;
    }
    std::error_code ec;
    return std::filesystem::exists(meshLoadHoldFile(), ec);
}

MeshSystem::~MeshSystem(){
    cancelAsyncModelLoads();
    if (!meshLoadHoldFile().empty()){
        std::lock_guard<std::mutex> lock(getMeshLoadHoldMutex());
        getMeshLoadHoldScenes().erase(scene);
    }
}

void MeshSystem::setImageDecodeMaxDimension(int dimension){
    g_imageDecodeMaxDimension = dimension;
}

std::string MeshSystem::getImageKeySuffix(){
    return g_imageDecodeMaxDimension > 0 ? "|max" + std::to_string(g_imageDecodeMaxDimension) : std::string();
}

void MeshSystem::applyDefaultObjMaterial(Submesh& submesh) {
    submesh.attributes.clear();
    submesh.material = Material();
    submesh.material.metallicFactor = 0.0f;
    submesh.material.alphaMode = MaterialAlphaMode::ALPHA_OPAQUE;
    submesh.textureShadow = false;
}

// Where each of a model's meshes starts in the primitive numbering a merged load produces. The
// submeshes are filled in ascending node order and then reversed, so a node's primitives form one
// run and the runs come in descending node order. The root mesh owns the numbering, and is empty
// unless the model is merged or single-node.
std::vector<std::pair<MeshComponent*, unsigned int>> MeshSystem::getSubmeshRuns(Entity entity, const ModelComponent& model) const {
    std::vector<std::pair<MeshComponent*, unsigned int>> runs;

    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
        runs.emplace_back(mesh, 0);
    }

    unsigned int base = 0;
    for (auto node = model.meshNodesMapping.rbegin(); node != model.meshNodesMapping.rend(); ++node) {
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(node->second)) {
            runs.emplace_back(mesh, base);
            base += mesh->numSubmeshes;
        }
    }

    return runs;
}

// Edited submeshes taken aside before a load rewrites them, under the ordinal of the primitive
// that built each one so they are found again wherever the load puts them.
MeshSystem::SubmeshOverrides MeshSystem::collectSubmeshOverrides(Entity entity, const ModelComponent& model) const {
    SubmeshOverrides overrides;

    for (auto const& [mesh, base] : getSubmeshRuns(entity, model)) {
        for (unsigned int i = 0; i < mesh->numSubmeshes; i++) {
            if (mesh->submeshes[i].overrideFields != 0) {
                // the saved copy must not hold the render's extra pass: a second owner makes
                // the load's destroyMesh take the pass for a component copy's and keep its
                // pipelines alive (only the material is restored from it)
                overrides.emplace(base + i, mesh->submeshes[i]).first->second.extraPass.reset();
            }
        }
    }

    return overrides;
}

// Puts the edited fields back on the meshes the load rebuilt.
void MeshSystem::applySubmeshOverrides(const SubmeshOverrides& overrides, Entity entity, const ModelComponent& model) const {
    if (overrides.empty()) {
        return;
    }

    for (auto const& [mesh, base] : getSubmeshRuns(entity, model)) {
        for (unsigned int i = 0; i < mesh->numSubmeshes; i++) {
            auto found = overrides.find(base + i);
            if (found != overrides.end()) {
                applySubmeshOverride(found->second, mesh->submeshes[i]);
            }
        }
    }
}

void MeshSystem::applySubmeshOverride(const Submesh& saved, Submesh& submesh) {
    const uint64_t fields = saved.overrideFields;

    Material& material = submesh.material;
    const Material& savedMaterial = saved.material;

    if (fields & SubmeshOverride_BaseColorFactor) material.baseColorFactor = savedMaterial.baseColorFactor;
    if (fields & SubmeshOverride_MetallicFactor)  material.metallicFactor  = savedMaterial.metallicFactor;
    if (fields & SubmeshOverride_RoughnessFactor) material.roughnessFactor = savedMaterial.roughnessFactor;
    if (fields & SubmeshOverride_AlphaCutoff)     material.alphaCutoff     = savedMaterial.alphaCutoff;
    if (fields & SubmeshOverride_FoliageTransmission) material.foliageTransmission = savedMaterial.foliageTransmission;
    if (fields & SubmeshOverride_EmissiveFactor)  material.emissiveFactor  = savedMaterial.emissiveFactor;
    if (fields & SubmeshOverride_AlphaMode)       material.alphaMode       = savedMaterial.alphaMode;
    if (fields & SubmeshOverride_MaterialName)    material.name            = savedMaterial.name;

    if (fields & SubmeshOverride_BaseColorTexture) {
        material.baseColorTexture = savedMaterial.baseColorTexture;
        material.baseColorTexCoord = savedMaterial.baseColorTexCoord;
    }
    if (fields & SubmeshOverride_EmissiveTexture) {
        material.emissiveTexture = savedMaterial.emissiveTexture;
        material.emissiveTexCoord = savedMaterial.emissiveTexCoord;
    }
    if (fields & SubmeshOverride_MetallicRoughnessTexture) {
        material.metallicRoughnessTexture = savedMaterial.metallicRoughnessTexture;
        material.metallicRoughnessTexCoord = savedMaterial.metallicRoughnessTexCoord;
    }
    if (fields & SubmeshOverride_OcclusionTexture) {
        material.occlusionTexture = savedMaterial.occlusionTexture;
        material.occlusionTexCoord = savedMaterial.occlusionTexCoord;
    }
    if (fields & SubmeshOverride_NormalTexture) {
        material.normalTexture = savedMaterial.normalTexture;
        material.normalTexCoord = savedMaterial.normalTexCoord;
    }

    if (fields & SubmeshOverride_Shader)         material.shader         = savedMaterial.shader;
    if (fields & SubmeshOverride_ShaderParams)   material.shaderParams   = savedMaterial.shaderParams;
    if (fields & SubmeshOverride_ShaderTextures) material.shaderTextures = savedMaterial.shaderTextures;
    if (fields & SubmeshOverride_ShaderFeatures) material.shaderFeatures = savedMaterial.shaderFeatures;
    if (fields & SubmeshOverride_ShaderGlobalBindings) material.shaderGlobalBindings = savedMaterial.shaderGlobalBindings;
    if (fields & SubmeshOverride_ShaderPalettes) material.shaderPalettes = savedMaterial.shaderPalettes;
    if (fields & SubmeshOverride_BlendMode)      material.blendMode      = savedMaterial.blendMode;
    if (fields & SubmeshOverride_CullMode)       material.cullMode       = savedMaterial.cullMode;
    if (fields & SubmeshOverride_DepthWrite)     material.depthWrite     = savedMaterial.depthWrite;
    if (fields & SubmeshOverride_DepthTest)      material.depthTest      = savedMaterial.depthTest;
    if (fields & SubmeshOverride_DepthOffset) {
        material.depthOffsetFactor = savedMaterial.depthOffsetFactor;
        material.depthOffsetUnits = savedMaterial.depthOffsetUnits;
    }
    if (fields & SubmeshOverride_SortPriority)   material.sortPriority   = savedMaterial.sortPriority;
    if (fields & SubmeshOverride_Lighting)       material.lighting       = savedMaterial.lighting;
    if (fields & SubmeshOverride_Stencil)        material.stencil        = savedMaterial.stencil;
    if (fields & SubmeshOverride_ColorWrite)     material.colorWrite     = savedMaterial.colorWrite;
    if (fields & SubmeshOverride_ExtraPass)      material.extraPass      = savedMaterial.extraPass;
    if (fields & SubmeshOverride_StochasticTiling) {
        material.stochasticTiling = savedMaterial.stochasticTiling;
        material.stochasticCellSize = savedMaterial.stochasticCellSize;
        material.stochasticBlendSharpness = savedMaterial.stochasticBlendSharpness;
        material.stochasticContrast = savedMaterial.stochasticContrast;
        material.stochasticRotation = savedMaterial.stochasticRotation;
    }
    if (fields & SubmeshOverride_EmissiveStrength) material.emissiveStrength = savedMaterial.emissiveStrength;
    if (fields & SubmeshOverride_PbrExtensions)  restorePbrExtensions(savedMaterial.pbr, material.pbr);

    if (fields & SubmeshOverride_FaceCulling)   submesh.faceCulling   = saved.faceCulling;
    if (fields & SubmeshOverride_TextureShadow) submesh.textureShadow = saved.textureShadow;
    if (fields & SubmeshOverride_PrimitiveType) submesh.primitiveType = saved.primitiveType;

    submesh.overrideFields = fields;
    submesh.needUpdateTexture = true;
}

void MeshSystem::restorePbrExtensions(const MaterialPbrExtensions& saved, OutOfLineValue<MaterialPbrExtensions>& loaded) {
    MaterialPbrExtensions result = saved;
    const MaterialPbrExtensions& fresh = loaded.get();
    for (int m = 0; m < (int)PbrMap::COUNT; m++) {
        const PbrMap map = (PbrMap)m;
        const Texture& freshMap = MaterialPbr::getMap(fresh, map);
        Texture& savedMap = MaterialPbr::getMap(result, map);
        if (!freshMap.empty() && savedMap.getId() == freshMap.getId())
            savedMap = freshMap;
    }
    loaded = result;
}

void MeshSystem::prepareSpriteMesh(MeshComponent& mesh){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.submeshes[0].hasTextureRect = true;
    mesh.submeshes[0].textureShadow = true;
    mesh.submeshes[0].faceCulling = false;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::TANGENT, 4);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    // shared by content (MeshBufferPool): sprites of one shape draw from one GPU buffer, a
    // changed sprite takes the buffer of its new bytes (RenderSystem::refreshImmutableBuffer); one
    // whose geometry kept changing has a dynamic buffer of its own, which it keeps
    BufferRender* render = mesh.buffer.getRender();
    if (!(mesh.buffer.getUsage() == BufferUsage::DYNAMIC && render->isCreated() && !render->isShared()))
        mesh.buffer.setUsage(BufferUsage::IMMUTABLE);
}

void MeshSystem::prepareMeshPolygonMesh(MeshComponent& mesh){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLE_STRIP;
    mesh.submeshes[0].faceCulling = false;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::TANGENT, 4);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);
}

void MeshSystem::prepareTilemapMesh(MeshComponent& mesh){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.submeshes[0].hasTextureRect = true;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::TANGENT, 4);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    mesh.buffer.setUsage(BufferUsage::DYNAMIC);
}

void MeshSystem::prepareTerrainMesh(MeshComponent& mesh){
    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
}

bool MeshSystem::usesTextureRect(const Submesh& submesh){
    Rect textureRect = submesh.textureRect; // Rect compares non-const
    return submesh.hasTextureRect || textureRect != Rect(0.0f, 0.0f, 1.0f, 1.0f);
}

bool MeshSystem::createSprite(SpriteComponent& sprite, MeshComponent& mesh, CameraComponent& camera){
    // Check texture loading state BEFORE clearing the buffer to avoid leaving
    // the mesh with an empty vertex buffer when the texture is still loading.
    Texture& mainTexture = mesh.submeshes[0].material.baseColorTexture;
    // before the first load, see RenderSystem::loadMesh
    mainTexture.setAlphaBorderAuto(true);

    unsigned int texWidth = 0;
    unsigned int texHeight = 0;

    if (!mainTexture.empty()){
        // A sprite refresh only needs the source dimensions. File-backed
        // textures retain those after their CPU pixels are released, while
        // load() would decode and resize the whole image again. This matters
        // especially for large sprite sheets which are already GPU-resident.
        texWidth = mainTexture.getWidth();
        texHeight = mainTexture.getHeight();

        if (texWidth == 0 || texHeight == 0){
            TextureLoadResult texResult = mainTexture.load();
            if (texResult.state == ResourceLoadState::Finished){
                texWidth = mainTexture.getWidth();
                texHeight = mainTexture.getHeight();
            }else if (texResult.state == ResourceLoadState::Loading){
                return false;
            }
        }
    }

    // a lattice deforming the sprite asks for a grid: a quad can only bend bilinearly
    const int grid = requestedSpriteGrid(mesh);
    const int previousGrid = builtSpriteGrid(mesh);

    prepareSpriteMesh(mesh);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);

    if (texWidth == 0 || texHeight == 0){
        texWidth = sprite.width;
        texHeight = sprite.height;
    }

    if (sprite.width == 0 && sprite.height == 0){
        sprite.width = texWidth;
        sprite.height = texHeight;
    }

    Vector2 pivotPos = Vector2(0, 0);

    if (sprite.pivotPreset == PivotPreset::BOTTOM_RIGHT){
        pivotPos.x = sprite.width;
    }else if (sprite.pivotPreset == PivotPreset::TOP_RIGHT){
        pivotPos.x = sprite.width;
        pivotPos.y = sprite.height;
    }else if (sprite.pivotPreset == PivotPreset::TOP_LEFT){
        pivotPos.y = sprite.height;
    }else if (sprite.pivotPreset == PivotPreset::TOP_CENTER){
        pivotPos.x = sprite.width / 2.0;
        pivotPos.y = sprite.height;
    }else if (sprite.pivotPreset == PivotPreset::BOTTOM_CENTER){
        pivotPos.x = sprite.width / 2.0;
    }else if (sprite.pivotPreset == PivotPreset::RIGHT_CENTER){
        pivotPos.x = sprite.width;
        pivotPos.y = sprite.height / 2.0;
    }else if (sprite.pivotPreset == PivotPreset::LEFT_CENTER){
        pivotPos.y = sprite.height / 2.0;
    }else if (sprite.pivotPreset == PivotPreset::CENTER || sprite.pivotPreset == PivotPreset::FRAME){
        // inline frames carry no pivot: From Frame is the centre on a legacy sprite
        pivotPos.x = sprite.width / 2.0;
        pivotPos.y = sprite.height / 2.0;
    }

    if (camera.type == CameraType::CAMERA_UI){
        pivotPos.y = sprite.height - pivotPos.y;
    }

    // the quad in world units: the pixel size over the Pixels Per Unit (S41 R6; 1 by default,
    // exact: the vertices are the pixel sizes then)
    const float ppu = resolvePixelsPerUnit(sprite.pixelsPerUnit);
    pivotPos = Vector2(pivotPos.x / ppu, pivotPos.y / ppu);
    const float quadWidth = (float)sprite.width / ppu;
    const float quadHeight = (float)sprite.height / ppu;

    if (grid > 1){
        createSpriteGrid(sprite, mesh, texWidth, texHeight, pivotPos, grid);

        // a GPU buffer keeps the size it was created with: another grid reloads the mesh
        if (mesh.loaded){
            if (previousGrid != 0 && previousGrid != grid){
                mesh.needReload = true;
            }else{
                mesh.needUpdateBuffer = true; // the bytes of the shared buffer changed
            }
        }
        return true;
    }

    mesh.buffer.addVector3(attVertex, Vector3(-pivotPos.x, -pivotPos.y, 0));
    mesh.buffer.addVector3(attVertex, Vector3(quadWidth-pivotPos.x, -pivotPos.y, 0));
    mesh.buffer.addVector3(attVertex, Vector3(quadWidth-pivotPos.x,  quadHeight-pivotPos.y, 0));
    mesh.buffer.addVector3(attVertex, Vector3(-pivotPos.x,  quadHeight-pivotPos.y, 0));

    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);

    float texCutRatioW = 0;
    float texCutRatioH = 0;
    if (texWidth != 0 && texHeight != 0){
        // in texels of the frame, as the shader scales these UVs by the texture rect
        // (mirrored frames have a negative width)
        const Rect& rect = mesh.submeshes[0].textureRect;
        float rectWidth = std::fabs(rect.getWidth()) > 0.0f ? std::fabs(rect.getWidth()) : 1.0f;
        float rectHeight = std::fabs(rect.getHeight()) > 0.0f ? std::fabs(rect.getHeight()) : 1.0f;
        texCutRatioW = sprite.textureScaleFactor / (texWidth * rectWidth);
        texCutRatioH = sprite.textureScaleFactor / (texHeight * rectHeight);
    }

    if (!sprite.flipY){ 
        mesh.buffer.addVector2(attTexcoord, Vector2(texCutRatioW, texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(1.0-texCutRatioW, texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(1.0-texCutRatioW, 1.0-texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(texCutRatioW, 1.0-texCutRatioH));
    }else{
        mesh.buffer.addVector2(attTexcoord, Vector2(texCutRatioW, 1.0-texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(1.0-texCutRatioW, 1.0-texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(1.0-texCutRatioW, texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(texCutRatioW, texCutRatioH));
    }

    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);

    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));

    // flat +Z quad with u along +X: constant tangent, so normal maps work with the
    // 2D light path. The bitangent points where v shrinks, up the image (OpenGL green),
    // which flipY turns toward +y (flatTangentHandedness)
    Attribute* attTangent = mesh.buffer.getAttribute(AttributeType::TANGENT);
    Vector4 tangent = Vector4(1.0f, 0.0f, 0.0f, flatTangentHandedness(sprite.flipY));

    mesh.buffer.addVector4(attTangent, tangent);
    mesh.buffer.addVector4(attTangent, tangent);
    mesh.buffer.addVector4(attTangent, tangent);
    mesh.buffer.addVector4(attTangent, tangent);

    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));


    static const uint16_t indices_array[] = {
        0,  1,  2,
        0,  2,  3,
    };

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        6, (char*)&indices_array[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded){
        // back from a lattice grid: the GPU buffer has the grid's size
        if (previousGrid > 1){
            mesh.needReload = true;
        }else{
            mesh.needUpdateBuffer = true; // the bytes of the shared buffer changed
        }
    }

    return true;
}

// The quad of createSprite as grid x grid cells: the same corners, UVs, texture rect, pivot,
// flip, normals, tangents and colour, interpolated across; corners land on the quad's values
// exactly, so the AABB is the quad's
void MeshSystem::createSpriteGrid(SpriteComponent& sprite, MeshComponent& mesh, unsigned int texWidth, unsigned int texHeight,
                                  Vector2 pivotPos, int grid){
    // world units, as createSprite's quad (pivotPos is in them already)
    const float ppu = resolvePixelsPerUnit(sprite.pixelsPerUnit);
    buildSpriteGrid(sprite, mesh, texWidth, texHeight, -pivotPos.x, -pivotPos.y,
                    (float)sprite.width / ppu - pivotPos.x, (float)sprite.height / ppu - pivotPos.y, grid);
}

void MeshSystem::buildSpriteGrid(SpriteComponent& sprite, MeshComponent& mesh, unsigned int texWidth, unsigned int texHeight,
                                 float x0, float y0, float x1, float y1, int grid, bool keepIndices){
    float texCutRatioW = 0;
    float texCutRatioH = 0;
    if (texWidth != 0 && texHeight != 0){
        // in texels of the frame, as the shader scales these UVs by the texture rect
        // (mirrored frames have a negative width)
        const Rect& rect = mesh.submeshes[0].textureRect;
        float rectWidth = std::fabs(rect.getWidth()) > 0.0f ? std::fabs(rect.getWidth()) : 1.0f;
        float rectHeight = std::fabs(rect.getHeight()) > 0.0f ? std::fabs(rect.getHeight()) : 1.0f;
        texCutRatioW = sprite.textureScaleFactor / (texWidth * rectWidth);
        texCutRatioH = sprite.textureScaleFactor / (texHeight * rectHeight);
    }
    const float u0 = texCutRatioW;
    const float u1 = (float)(1.0 - texCutRatioW);
    // flipY swaps the V of the bottom and top rows
    const float vBottom = sprite.flipY ? (float)(1.0 - texCutRatioH) : texCutRatioH;
    const float vTop = sprite.flipY ? texCutRatioH : (float)(1.0 - texCutRatioH);

    // exact at both ends: (1 - t) * a + t * b
    auto lerp = [](float a, float b, float t){
        return (1.0f - t) * a + t * b;
    };

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attTangent = mesh.buffer.getAttribute(AttributeType::TANGENT);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    // the quad's tangent: the bitangent up the image (flatTangentHandedness)
    const Vector4 tangent = Vector4(1.0f, 0.0f, 0.0f, flatTangentHandedness(sprite.flipY));

    // rows from the bottom (y0) up, x fastest
    for (int row = 0; row <= grid; row++){
        const float ty = (float)row / (float)grid;
        for (int column = 0; column <= grid; column++){
            const float tx = (float)column / (float)grid;
            mesh.buffer.addVector3(attVertex, Vector3(lerp(x0, x1, tx), lerp(y0, y1, ty), 0));
            mesh.buffer.addVector2(attTexcoord, Vector2(lerp(u0, u1, tx), lerp(vBottom, vTop, ty)));
            mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
            mesh.buffer.addVector4(attTangent, tangent);
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // the grid's bounds are its corners' (calculateMeshAABB's result, without its buffer map: a
    // frame change allocates nothing, S45 R26)
    auto cornerBounds = [&](){
        mesh.verticesAABB = AABB(std::min(x0, x1), std::min(y0, y1), 0.0f, std::max(x0, x1), std::max(y0, y1), 0.0f);
        mesh.bonesAABB.clear();
        mesh.skinnedAABB.setNull();
        mesh.aabb = mesh.verticesAABB;
        mesh.needUpdateAABB = true;
        mesh.pickGeometryHash = 0;
    };
    if (keepIndices){
        cornerBounds();
        return;
    }
    // the quad: its indices without an allocation
    if (grid == 1){
        static const uint16_t quadIndices[] = {0, 1, 3, 0, 3, 2};
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), 6, (char*)&quadIndices[0], sizeof(uint16_t));
        cornerBounds();
        return;
    }

    // two triangles per cell, wound as the quad's (0, 1, 2) (0, 2, 3)
    std::vector<uint16_t> indices;
    indices.reserve((size_t)grid * grid * 6);
    const int stride = grid + 1;
    for (int row = 0; row < grid; row++){
        for (int column = 0; column < grid; column++){
            const uint16_t v00 = (uint16_t)(row * stride + column);
            const uint16_t v10 = (uint16_t)(v00 + 1);
            const uint16_t v11 = (uint16_t)(v00 + stride + 1);
            const uint16_t v01 = (uint16_t)(v00 + stride);
            indices.insert(indices.end(), {v00, v10, v11, v00, v11, v01});
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);
}

// The 9-slice of a sheet frame (S45 R6): 16 vertices and 54 indices on the sprite's own buffer,
// with the quad's normals, tangent and colour
void MeshSystem::buildSpriteSliced(SpriteComponent& sprite, MeshComponent& mesh, const SpriteSlicedMesh& sliced){
    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attTangent = mesh.buffer.getAttribute(AttributeType::TANGENT);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);
    const Vector4 tangent = Vector4(1.0f, 0.0f, 0.0f, flatTangentHandedness(sprite.flipY));
    for (int row = 0; row < 4; row++){
        for (int column = 0; column < 4; column++){
            mesh.buffer.addVector3(attVertex, Vector3(sliced.x[column], sliced.y[row], 0));
            mesh.buffer.addVector2(attTexcoord, Vector2(sliced.u[column], sliced.v[row]));
            mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
            mesh.buffer.addVector4(attTangent, tangent);
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }
    mesh.indices.clear();
    mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), SPRITE_SLICED_INDICES,
                           (char*)spriteSlicedIndices(), sizeof(uint16_t));
    calculateSpriteAABB(mesh);
}

// calculateMeshAABB for a sprite's own buffer (no external buffers, no bones), without its buffer
// map: a sheet sprite's frame change allocates nothing (S45 R26)
void MeshSystem::calculateSpriteAABB(MeshComponent& mesh){
    mesh.verticesAABB.setNull();
    mesh.bonesAABB.clear();
    mesh.skinnedAABB.setNull();
    Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
    if (position && position->getDataType() == AttributeDataType::FLOAT){
        const int count = int(position->getCount());
        for (int v = 0; v < count; v++){
            mesh.verticesAABB.merge(mesh.buffer.getVector3(position, v));
        }
    }
    if (mesh.verticesAABB.isNull())
        mesh.verticesAABB = AABB::ZERO;
    mesh.aabb = mesh.verticesAABB;
    mesh.needUpdateAABB = true;
    mesh.pickGeometryHash = 0;
}

bool MeshSystem::createMeshPolygon(MeshPolygonComponent& polygon, MeshComponent& mesh){
    prepareMeshPolygonMesh(mesh);

    // flat +Z polygon with u along +X (see texcoord generation below); the bitangent
    // points where v shrinks, up the image, which flipY turns toward +y
    // (flatTangentHandedness)
    Vector4 tangent = Vector4(1.0f, 0.0f, 0.0f, flatTangentHandedness(polygon.flipY));

    for (int i = 0; i < polygon.points.size(); i++){
        mesh.buffer.addVector3(AttributeType::POSITION, polygon.points[i].position);
        mesh.buffer.addVector3(AttributeType::NORMAL, Vector3(0.0f, 0.0f, 1.0f));
        mesh.buffer.addVector4(AttributeType::TANGENT, tangent);
        mesh.buffer.addVector4(AttributeType::COLOR, polygon.points[i].color);
    }

    // Generation texcoords (lowest(), not min(): min() is the smallest positive float, so a
    // polygon left of or below its origin measured to 0 instead of to its own edge)
    float min_X = std::numeric_limits<float>::max();
    float max_X = std::numeric_limits<float>::lowest();
    float min_Y = std::numeric_limits<float>::max();
    float max_Y = std::numeric_limits<float>::lowest();

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);

    for (unsigned int i = 0; i < mesh.buffer.getCount(); i++){
        min_X = fmin(min_X, mesh.buffer.getFloat(attVertex, i, 0));
        min_Y = fmin(min_Y, mesh.buffer.getFloat(attVertex, i, 1));
        max_X = fmax(max_X, mesh.buffer.getFloat(attVertex, i, 0));
        max_Y = fmax(max_Y, mesh.buffer.getFloat(attVertex, i, 1));
    }

    double k_X = 1/(max_X - min_X);
    double k_Y = 1/(max_Y - min_Y);

    float u = 0;
    float v = 0;

    for ( unsigned int i = 0; i < mesh.buffer.getCount(); i++){
        u = (mesh.buffer.getFloat(attVertex, i, 0) - min_X) * k_X;
        v = (mesh.buffer.getFloat(attVertex, i, 1) - min_Y) * k_Y;
        if (polygon.flipY) {
            mesh.buffer.addVector2(AttributeType::TEXCOORD1, Vector2(u, 1.0 - v));
        }else{
            mesh.buffer.addVector2(AttributeType::TEXCOORD1, Vector2(u, v));
        }
    }

    polygon.width = (int)(max_X - min_X);
    polygon.height = (int)(max_Y - min_Y);

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;

    return true;
}

bool MeshSystem::createTilemap(TilemapComponent& tilemap, MeshComponent& mesh){
    // Pre-check all tile textures BEFORE clearing the buffer to avoid leaving
    // the mesh with an empty vertex buffer when textures are still loading.
    //
    // Only force a CPU load while the texture dimensions are still unknown: tile
    // meshes only need the texture width/height (for UV normalization), never the
    // pixel data. Those dimensions survive releaseDataAfterLoad, so once a texture
    // has loaded once getWidth() keeps returning them without touching the pool.
    // Calling load() unconditionally would instead re-read every already-uploaded
    // (and released) tile texture from disk on each rebuild, spinning an endless
    // load loop.
    // before the first load, see RenderSystem::loadMesh
    for (int s = 0; s < mesh.numSubmeshes; s++){
        mesh.submeshes[s].material.baseColorTexture.setAlphaBorderAuto(true);
    }

    unsigned int preReserveTiles = tilemap.reserveTiles;
    for (int i = 0; i < (int)tilemap.numTiles; i++){
        if (tilemap.tiles[i].width == 0 && tilemap.tiles[i].height == 0 && preReserveTiles == 0){
            continue;
        }
        if (tilemap.tiles[i].rectId < 0 || (unsigned int)tilemap.tiles[i].rectId >= tilemap.numTilesRect){
            continue;
        }
        if (preReserveTiles > 0){
            preReserveTiles--;
        }
        TileRectData& rectData = tilemap.tilesRect[tilemap.tiles[i].rectId];
        if (rectData.submeshId < 0 || (unsigned int)rectData.submeshId >= mesh.numSubmeshes){
            continue;
        }
        Texture& texture = mesh.submeshes[rectData.submeshId].material.baseColorTexture;
        Texture& mainTexture = mesh.submeshes[0].material.baseColorTexture;
        Texture* tileTexture = !texture.empty() ? &texture : (!mainTexture.empty() ? &mainTexture : nullptr);
        if (tileTexture && tileTexture->getWidth() == 0){
            TextureLoadResult texResult = tileTexture->load();
            if (texResult.state == ResourceLoadState::Loading){
                return false;
            }
        }
    }

    prepareTilemapMesh(mesh);

    // Chunking reorders the index data whenever a tile crosses a chunk boundary, so
    // the indices must be updatable like the vertices — an immutable index buffer is
    // skipped by the needUpdateBuffer upload and would leave the GPU on the old order
    // while the chunk offsets already describe the new one.
    const bool indexUsageChanged = mesh.indices.getUsage() != BufferUsage::DYNAMIC;
    mesh.indices.setUsage(BufferUsage::DYNAMIC);

    tilemap.width = 0;
    tilemap.height = 0;

    std::vector<std::vector<uint16_t>> indexMap;
    indexMap.resize(mesh.numSubmeshes);

    std::vector<TilemapAcceptedTile> acceptedTiles;
    acceptedTiles.reserve(tilemap.numTiles);

    // the quads in world units: the tiles' pixels over the Pixels Per Unit (S41 R6); under a
    // Pixel Perfect Camera each edge is rounded to the texel grid on its own (R13), so the
    // edge two neighbours share is bit for bit the same value and the vertex snap moves both
    const float ppu = resolvePixelsPerUnit(tilemap.pixelsPerUnit);
    auto tileEdge = [&tilemap, ppu](float pixels){
        return (tilemap.roundTileEdges ? pixelart::snap(pixels) : pixels) / ppu;
    };

    unsigned int numTiles = 0;
    unsigned int reserveTiles = tilemap.reserveTiles;

    for (int i = 0; i < (int)tilemap.numTiles; i++){

        if (tilemap.tiles[i].width == 0 && tilemap.tiles[i].height == 0 && reserveTiles == 0){
            continue;
        }

        if (tilemap.tiles[i].rectId < 0 || (unsigned int)tilemap.tiles[i].rectId >= tilemap.numTilesRect){
            continue;
        }

        // A quad past this point can no longer be addressed by the 16-bit index
        // buffer, so stop instead of wrapping vertex indices into other tiles.
        if (numTiles >= MAX_TILEMAP_RENDERED_TILES){
            Log::error("Tilemap exceeds the maximum of %i rendered tiles, remaining tiles were dropped", MAX_TILEMAP_RENDERED_TILES);
            break;
        }

        if (reserveTiles > 0){
            reserveTiles--;
        }

        numTiles++;

        // Clamp tile position to non-negative — negative local coords are not supported
        if (tilemap.tiles[i].position.x < 0) tilemap.tiles[i].position.x = 0;
        if (tilemap.tiles[i].position.y < 0) tilemap.tiles[i].position.y = 0;

        Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
        const float tileX0 = tileEdge(tilemap.tiles[i].position.x);
        const float tileX1 = tileEdge(tilemap.tiles[i].position.x + tilemap.tiles[i].width);
        const float tileY0 = tileEdge(tilemap.tiles[i].position.y);
        const float tileY1 = tileEdge(tilemap.tiles[i].position.y + tilemap.tiles[i].height);
        mesh.buffer.addVector3(attVertex, Vector3(tileX0, tileY0, 0));
        mesh.buffer.addVector3(attVertex, Vector3(tileX1, tileY0, 0));
        mesh.buffer.addVector3(attVertex, Vector3(tileX1, tileY1, 0));
        mesh.buffer.addVector3(attVertex, Vector3(tileX0, tileY1, 0));

        if (tilemap.width < tilemap.tiles[i].position.x + tilemap.tiles[i].width)
            tilemap.width = static_cast<unsigned int>(tilemap.tiles[i].position.x + tilemap.tiles[i].width);
        if (tilemap.height < tilemap.tiles[i].position.y + tilemap.tiles[i].height)
            tilemap.height = static_cast<unsigned int>(tilemap.tiles[i].position.y + tilemap.tiles[i].height);

        TileRectData& rectData = tilemap.tilesRect[tilemap.tiles[i].rectId];
        int submeshId = (rectData.submeshId >= 0 && (unsigned int)rectData.submeshId < mesh.numSubmeshes) ? rectData.submeshId : 0;
        Rect tileRect = rectData.rect;

        Texture& texture = mesh.submeshes[submeshId].material.baseColorTexture;
        Texture& mainTexture = mesh.submeshes[0].material.baseColorTexture;
        Texture* tileTexture = !texture.empty() ? &texture : (!mainTexture.empty() ? &mainTexture : nullptr);

        unsigned int texWidth = 0;
        unsigned int texHeight = 0;
        if (tileTexture){
            // Dimensions are retained even after releaseDataAfterLoad frees the
            // pixel buffer, so read them directly instead of forcing a reload.
            texWidth = tileTexture->getWidth();
            texHeight = tileTexture->getHeight();
            if (texWidth != 0 && texHeight != 0){
                tileRect = normalizeTileRect(tileRect, texWidth, texHeight);
            }
        }

        float texCutRatioW = 0;
        float texCutRatioH = 0;
        if (texWidth != 0 && texHeight != 0){
            texCutRatioW = 1.0 / texWidth * tilemap.textureScaleFactor;
            texCutRatioH = 1.0 / texHeight * tilemap.textureScaleFactor;
        }

        Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        if (tilemap.flipY){
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+texCutRatioW, tileRect.getY()+tileRect.getHeight()-texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+tileRect.getWidth()-texCutRatioW, tileRect.getY()+tileRect.getHeight()-texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+tileRect.getWidth()-texCutRatioW, tileRect.getY()+texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+texCutRatioW, tileRect.getY()+texCutRatioH));
        }else{
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+texCutRatioW, tileRect.getY()+texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+tileRect.getWidth()-texCutRatioW, tileRect.getY()+texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+tileRect.getWidth()-texCutRatioW, tileRect.getY()+tileRect.getHeight()-texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+texCutRatioW, tileRect.getY()+tileRect.getHeight()-texCutRatioH));
        }

        Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
        mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
        mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
        mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));

        // flat +Z tiles: the tangent points where u grows, the bitangent where v shrinks,
        // up the image, which flipY turns toward +y (flatTangentHandedness). A tile whose
        // rect has a negative width or height (mirrored) has the mirror in its UVs: u then
        // grows toward -x, and v turns too
        const float mirrorX = tileRect.getWidth() < 0.0f ? -1.0f : 1.0f;
        const float mirrorY = tileRect.getHeight() < 0.0f ? -1.0f : 1.0f;
        Attribute* attTangent = mesh.buffer.getAttribute(AttributeType::TANGENT);
        Vector4 tangent = Vector4(mirrorX, 0.0f, 0.0f, flatTangentHandedness(tilemap.flipY) * mirrorX * mirrorY);
        mesh.buffer.addVector4(attTangent, tangent);
        mesh.buffer.addVector4(attTangent, tangent);
        mesh.buffer.addVector4(attTangent, tangent);
        mesh.buffer.addVector4(attTangent, tangent);

        Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));

        // Indices must address the vertex buffer by the tile's position in it, which
        // only matches the loop counter while no tile is skipped — an invalidated
        // rectId or an empty tile past the reserve budget shifts every later tile.
        const unsigned int vertexBase = (numTiles - 1) * 4;
        acceptedTiles.push_back({vertexBase, (unsigned int)submeshId, 0,
            AABB(Vector3(tileX0, tileY0, 0), Vector3(tileX1, tileY1, 0))});

    }

    buildTilemapChunks(tilemap, acceptedTiles, indexMap);

    // Each submesh's index range is baked into its GPU bindings (offset) and vertex
    // count when the mesh loads, so any change in how the tiles are spread over the
    // submeshes needs a reload — even when the total tile count stayed the same,
    // like a tile switching to a rect of a different texture.
    std::vector<size_t> previousIndexCounts(mesh.numSubmeshes, 0);
    for (int i = 0; i < mesh.numSubmeshes; i++){
        auto indexAttribute = mesh.submeshes[i].attributes.find(AttributeType::INDEX);
        if (indexAttribute != mesh.submeshes[i].attributes.end()){
            previousIndexCounts[i] = indexAttribute->second.getCount();
        }
    }

    mesh.indices.clear();

    for (int i = 0; i < mesh.numSubmeshes; i++){
        addSubmeshAttribute(mesh.submeshes[i], "indices", AttributeType::INDEX, 1, AttributeDataType::UNSIGNED_SHORT, indexMap[i].size(), mesh.indices.getCount() * sizeof(uint16_t), false);

        // a submesh without indices draws nothing (see RenderSystem::loadMesh)
        if (indexMap[i].size() > 0){
            mesh.indices.setValues(mesh.indices.getCount(), mesh.indices.getAttribute(AttributeType::INDEX), indexMap[i].size(), (char*)&indexMap[i].front(), sizeof(uint16_t));
            mesh.indices.setRenderAttributes(false);
        }
    }

    calculateMeshAABB(mesh);

    if (mesh.loaded){
        bool indexLayoutChanged = tilemap.renderedTiles != numTiles;
        for (int i = 0; i < mesh.numSubmeshes && !indexLayoutChanged; i++){
            indexLayoutChanged = previousIndexCounts[i] != indexMap[i].size();
        }

        // a GPU buffer keeps the usage it was created with, so a mesh already loaded
        // with immutable indices has to reload before it can be updated
        if (indexLayoutChanged || indexUsageChanged){
            mesh.needReload = true;
        }else{
            mesh.needUpdateBuffer = true; // buffer is not immutable
        }
    }
    tilemap.renderedTiles = numTiles;

    return true;
}

float MeshSystem::flatTangentHandedness(bool flipY){
    // B = cross(+Z, +X) * w = +Y * w: up the image is +y with flipY (the bottom vertices take
    // the image's bottom, v near 1), -y without it (v grows with +y)
    return flipY ? 1.0f : -1.0f;
}

void MeshSystem::changeFlipY(bool& flipY, CameraComponent& camera, MeshComponent& mesh){
    // framebuffer textures need no special case: offscreen passes render with a
    // Y-flipped projection on GL, so they are top-left origin on every backend
    flipY = false;
    if (camera.type != CameraType::CAMERA_UI){
        flipY = !flipY;
    }
}

Rect MeshSystem::normalizeTileRect(Rect tileRect, unsigned int texWidth, unsigned int texHeight){
    Rect normalized = tileRect;

    if (!tileRect.isNormalized()){
        normalized.setRect((tileRect.getX()) / (float) texWidth,
                            (tileRect.getY()) / (float) texHeight,
                            (tileRect.getWidth()) / (float) texWidth,
                            (tileRect.getHeight()) / (float) texHeight);
    }

    return normalized;
}

// An async texture load has no size yet, so the frame is kept in pixels and converted
// by createOrUpdateSprite once the size arrives
void MeshSystem::setSpriteFrameRect(MeshComponent& mesh, SpriteComponent& sprite, Rect frameRect){
    // The first slot exists before createSprite sets numSubmeshes, so scripts can
    // select a frame while configuring a newly constructed sprite.

    // a legacy sprite drawn from an atlas page (S45 R8): its frames are the source texture's
    if (!frameRect.isNormalized() && sprite.inlineAtlas && !sprite.inlineAtlas->page.empty() &&
        mesh.submeshes[0].material.baseColorTexture.getPath() == sprite.inlineAtlas->page && sprite.inlineAtlas->sourceWidth > 0){
        frameRect = normalizeTileRect(frameRect, (unsigned int)sprite.inlineAtlas->sourceWidth, (unsigned int)sprite.inlineAtlas->sourceHeight);
    }
    if (!frameRect.isNormalized()){
        Texture& texture = mesh.submeshes[0].material.baseColorTexture;

        if (!texture.empty() && texture.getWidth() == 0 && texture.getHeight() == 0){
            texture.load();
        }

        if (texture.getWidth() == 0 || texture.getHeight() == 0){
            sprite.pendingFrameRect = frameRect;
            sprite.needUpdateFrameRect = true;
            return;
        }

        frameRect = normalizeTileRect(frameRect, texture.getWidth(), texture.getHeight());
    }

    if (changesSpriteInset(sprite, mesh.submeshes[0].textureRect, frameRect)){
        sprite.needUpdateSprite = true;
    }

    sprite.needUpdateFrameRect = false;
    mesh.submeshes[0].textureRect = frameRect;
}

namespace{
    // the page shows the source's frame as last mapped (nobody wrote another rect since)
    const SpriteInlineAtlas* drawnFromPage(const SpriteComponent& sprite, const MeshComponent& mesh){
        const SpriteInlineAtlas* atlas = sprite.inlineAtlas.get();
        if (!atlas || atlas->page.empty() || mesh.submeshes[0].material.baseColorTexture.getPath() != atlas->page) return nullptr;
        return atlas;
    }
}

Rect MeshSystem::spriteSourceRect(const SpriteComponent& sprite, const MeshComponent& mesh){
    const SpriteInlineAtlas* atlas = drawnFromPage(sprite, mesh);
    Rect current = mesh.submeshes[0].textureRect;
    if (atlas && !(current != atlas->pageRect)) return atlas->sourceRect;
    return current;
}

std::string MeshSystem::spriteSourceTexture(const SpriteComponent& sprite, const MeshComponent& mesh){
    const SpriteInlineAtlas* atlas = drawnFromPage(sprite, mesh);
    return atlas ? atlas->source : mesh.submeshes[0].material.baseColorTexture.getPath();
}

void MeshSystem::spriteSourceSize(const SpriteComponent& sprite, const MeshComponent& mesh, float& width, float& height){
    if (const SpriteInlineAtlas* atlas = drawnFromPage(sprite, mesh)){
        width = (float)atlas->sourceWidth;
        height = (float)atlas->sourceHeight;
    }
}

// createSprite sizes the inset by the frame, mirrored frames only flip the sign
bool MeshSystem::changesSpriteInset(const SpriteComponent& sprite, const Rect& oldRect, const Rect& newRect){
    if (sprite.textureScaleFactor == 0)
        return false;

    return std::fabs(newRect.getWidth()) != std::fabs(oldRect.getWidth()) ||
           std::fabs(newRect.getHeight()) != std::fabs(oldRect.getHeight());
}

std::vector<float> MeshSystem::getCylinderSideNormals(float baseRadius, float topRadius, float height, float slices){
    float sectorStep = 2 * M_PI / slices;
    float sectorAngle;  // radian

    // compute the normal vector at 0 degree first
    // tanA = (baseRadius-topRadius) / height
    float yAngle = atan2(baseRadius - topRadius, height);
    float x0 = cos(yAngle);     // nx
    float y0 = sin(yAngle);     // ny

    // rotate (x0,y0,0) per sector angle
    std::vector<float> normals;
    for(int i = 0; i <= slices; ++i)
    {
        sectorAngle = i * sectorStep;
        normals.push_back(sin(sectorAngle)*x0);    // nx
        normals.push_back(y0);                     // ny
        normals.push_back(-cos(sectorAngle)*x0);   // nz
    }

    return normals;
}

// generate 3D vertices of a unit circle on XZ plane
std::vector<float> MeshSystem::buildUnitCircleVertices(float slices){
    float sectorStep = 2 * M_PI / slices;
    float sectorAngle;  // radian

    std::vector<float> unitCircleVertices;

    for(int i = 0; i <= slices; ++i){
        sectorAngle = i * sectorStep;
        unitCircleVertices.push_back(sin(sectorAngle));  // x
        unitCircleVertices.push_back(0);                 // y
        unitCircleVertices.push_back(-cos(sectorAngle)); // z
    }

    return unitCircleVertices;
}

std::string MeshSystem::readFileToString(const char* filename){
    Data filedata;

    if (filedata.open(filename) != FileErrors::FILEDATA_OK){
        Log::error("Model file not found: %s", filename);
        return "";
    }
    filedata.seek(0);
    return filedata.readString(filedata.length());
}

bool MeshSystem::fileExists(const std::string &abs_filename, void *) {
    // tinygltf checks here before loading external buffers and images
    if (ResourcePack::contains(abs_filename)) {
        return true;
    }

    File df;
    return df.open(abs_filename.c_str()) == FileErrors::FILEDATA_OK;
}

bool MeshSystem::normalizeGLTF(tinygltf::Model& model, const std::string& filename, std::string* err){
    GltfNormalizeResult result;
    if (GltfNormalize::run(model, filename, result))
        return true;
    if (err)
        *err = result.firstError();
    return false;
}

bool MeshSystem::readGLTFFile(tinygltf::TinyGLTF& loader, tinygltf::Model* model, std::string* err, std::string* warn, const std::string& filename){
    std::string ext = FileData::getFilePathExtension(filename);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    // a cooked .glb or an FBX's import: read from there, resolved against the source's directory
    const std::string read = getImportedModelPath(filename);
    if (read != filename){
        std::vector<unsigned char> data;
        if (!readWholeFile(&data, err, read, nullptr) || data.empty()){
            return false;
        }
        if (isGlbData(data.data(), data.size())){
            return loader.LoadBinaryFromMemory(model, err, warn, data.data(), static_cast<unsigned int>(data.size()), FileData::getBaseDir(filename));
        }
        return loader.LoadASCIIFromString(model, err, warn, reinterpret_cast<const char*>(data.data()), static_cast<unsigned int>(data.size()),
                                          FileData::getBaseDir(filename));
    }
    if (ext == "glb"){
        return loader.LoadBinaryFromFile(model, err, warn, filename);
    }
    return loader.LoadASCIIFromFile(model, err, warn, filename);
}

bool MeshSystem::loadGLTFFile(tinygltf::TinyGLTF& loader, tinygltf::Model* model, std::string* err, std::string* warn, const std::string& filename){
    const bool loaded = readGLTFFile(loader, model, err, warn, filename);
    // what the parse gives, as core glTF (docs/specs/S53-model-import-cook.md R1)
    if (!loaded || (err && !err->empty()))
        return loaded;
    return normalizeGLTF(*model, filename, err);
}

bool MeshSystem::parseGLTFFile(const std::string& filename, tinygltf::Model& model, std::string* err){
    tinygltf::TinyGLTF loader;
    // External images are never read: the caller wants nodes and animations. A missing image is
    // only a warning to tinygltf (the image keeps its uri), buffers load as usual.
    auto noImages = [](const std::string& path, void* userData) -> bool {
        std::string ext = FileData::getFilePathExtension(path);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        static const char* const imageExtensions[] = {"png", "jpg", "jpeg", "webp", "ktx", "ktx2", "basis", "dds", "tga", "bmp", "gif", "hdr", "exr"};
        for (const char* image : imageExtensions){
            if (ext == image) return false;
        }
        return fileExists(path, userData);
    };
    loader.SetFsCallbacks({noImages, &tinygltf::ExpandFilePath, &readWholeFile, &tinygltf::WriteWholeFile, &getFileSizeInBytes, nullptr});
    loader.SetImagesAsIs(true); // never decoded either

    std::string error;
    std::string warn;
    const bool loaded = loadGLTFFile(loader, &model, &error, &warn, filename);
    if (!loaded || !error.empty()){
        if (err) *err = error.empty() ? "cannot load model " + filename : error;
        return false;
    }
    return true;
}

bool MeshSystem::readWholeFile(std::vector<unsigned char> *out, std::string *err, const std::string &filepath, void *) {
    Data filedata;

    if (filedata.open(filepath.c_str()) != FileErrors::FILEDATA_OK){
        if (err) {
            (*err) += "File open error : " + filepath + "\n";
        }
        Log::error("Model file not found: %s", filepath.c_str());
        return false;
    }

    filedata.seek(0);
    std::istringstream f(filedata.readString(filedata.length()));

    if (!f) {
        if (err) {
            (*err) += "File open error : " + filepath + "\n";
        }
        return false;
    }

    f.seekg(0, f.end);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0, f.beg);

    if (int(sz) < 0) {
        if (err) {
            (*err) += "Invalid file size : " + filepath +
                      " (does the path point to a directory?)";
        }
        return false;
    } else if (sz == 0) {
        if (err) {
            (*err) += "File is empty : " + filepath + "\n";
        }
        return false;
    }

    out->resize(sz);
    f.read(reinterpret_cast<char *>(&out->at(0)),
           static_cast<std::streamsize>(sz));

    return true;
}

bool MeshSystem::getFileSizeInBytes(size_t *filesize_out, std::string *err, const std::string &filepath, void *userdata) {
  (void)userdata;

    Data filedata;

    if (filedata.open(filepath.c_str()) != FileErrors::FILEDATA_OK){
        if (err) {
            (*err) += "File open error : " + filepath + "\n";
        }
        Log::error("Model file not found: %s", filepath.c_str());
        return false;
    }

    (*filesize_out) = static_cast<size_t>(filedata.length());

    return true;
}

std::string MeshSystem::getAsyncModelLoadScenePrefix(const Scene* scene){
    std::ostringstream key;
    key << reinterpret_cast<uintptr_t>(scene) << '|';
    return key.str();
}

std::string MeshSystem::getAsyncModelLoadKey(const Scene* scene, const std::string& filename){
    return getAsyncModelLoadScenePrefix(scene) + getModelFilenameKey(filename);
}

std::string MeshSystem::getAsyncModelLoadKey(const std::string& filename) const{
    return getAsyncModelLoadKey(scene, filename);
}

bool MeshSystem::isAsyncModelLoadPending(const std::string& filename) const{
    return isModelPreloading(filename) || isAsyncModelLoadPending(scene, filename);
}

bool MeshSystem::isAsyncModelLoadPending(const Scene* scene, const std::string& filename){
    const std::string key = getAsyncModelLoadKey(scene, filename);
    std::lock_guard<std::mutex> lock(getAsyncModelMutex());
    return getPendingModelLoads().count(key) > 0;
}

bool MeshSystem::isModelPreloading(const std::string& filename){
    return isAsyncModelLoadPending(nullptr, filename);
}

std::vector<Entity> MeshSystem::getModelParts(Entity model) const{
    std::vector<Entity> parts;
    const ModelComponent* modelcomp = scene->findComponent<ModelComponent>(model);
    if (!modelcomp)
        return parts;
    for (const auto& node : modelcomp->meshNodesMapping){
        if (node.second != model && scene->isEntityCreated(node.second) && scene->findComponent<MeshComponent>(node.second))
            parts.push_back(node.second);
    }
    return parts;
}

bool MeshSystem::hasPendingAsyncModelLoads() const{
    const std::string scenePrefix = getAsyncModelLoadScenePrefix(scene);
    std::lock_guard<std::mutex> lock(getAsyncModelMutex());
    auto& pendingModelLoads = getPendingModelLoads();
    for (const auto& [key, task] : pendingModelLoads) {
        if (key.rfind(scenePrefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

void MeshSystem::cancelAsyncModelLoads(){
    const std::string scenePrefix = getAsyncModelLoadScenePrefix(scene);
    std::vector<uint64_t> buildIds;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        for (auto it = pendingModelLoads.begin(); it != pendingModelLoads.end();) {
            if (it->first.rfind(scenePrefix, 0) == 0) {
                buildIds.push_back(std::hash<std::string>{}(it->first));
                it->second.cancel();
                it->second.wait();
                it = pendingModelLoads.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (uint64_t buildId : buildIds) {
        ResourceProgress::failBuild(buildId);
    }
}

void MeshSystem::cancelAllAsyncModelLoads(){
    std::vector<uint64_t> buildIds;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        buildIds.reserve(pendingModelLoads.size());
        for (auto& [key, task] : pendingModelLoads) {
            buildIds.push_back(std::hash<std::string>{}(key));
            task.cancel();
            task.wait();
        }
        pendingModelLoads.clear();
    }
    for (uint64_t buildId : buildIds) {
        ResourceProgress::failBuild(buildId);
    }
}

bool MeshSystem::isAsyncModelLoadPending(Entity entity, const std::string& filename) const{
    (void)entity;
    return isAsyncModelLoadPending(scene, filename);
}

void MeshSystem::cancelAsyncModelLoad(Entity entity, const std::string& filename){
    (void)entity;
    cancelAsyncModelLoadByKey(getAsyncModelLoadKey(filename));
}

void MeshSystem::cancelAsyncModelLoadByKey(const std::string& key){
    const uint64_t buildId = std::hash<std::string>{}(key);
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        auto it = pendingModelLoads.find(key);
        if (it != pendingModelLoads.end()) {
            it->second.cancel();
            it->second.wait();
            pendingModelLoads.erase(it);
            erased = true;
        }
    }
    if (erased){
        ResourceProgress::failBuild(buildId);
    }
}

void MeshSystem::collectUnclaimedAsyncModelLoads(){
    const std::string scenePrefix = getAsyncModelLoadScenePrefix(scene);
    std::vector<std::string> unclaimed;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        // counts restart for a load taken (or cancelled) and started again
        for (auto it = unclaimedLoadFrames.begin(); it != unclaimedLoadFrames.end();) {
            auto pending = pendingModelLoads.find(it->first);
            if (pending == pendingModelLoads.end() || !pending->second.isReady()) {
                it = unclaimedLoadFrames.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [key, task] : pendingModelLoads) {
            if (key.rfind(scenePrefix, 0) != 0 || !task.isReady()) {
                continue;
            }
            if (++unclaimedLoadFrames[key] >= UNCLAIMED_LOAD_FRAMES) {
                unclaimed.push_back(key);
            }
        }
    }
    for (const std::string& key : unclaimed) {
        unclaimedLoadFrames.erase(key);
        Log::debug("Model load nobody waits for any more dropped: %s", key.substr(scenePrefix.size()).c_str());
        cancelAsyncModelLoadByKey(key);
    }
}

// Decodes one image that SetImagesAsIs left still encoded in image.image.
void MeshSystem::decodeGLTFImage(tinygltf::Image& image, size_t index, int maxDimension) {
    if (image.image.empty()) {
        return; // no encoded bytes (e.g. external/unsupported image)
    }

    std::vector<unsigned char> encoded = std::move(image.image);
    image.image.clear();
    std::string err;
    std::string warn;
    // what stb does not read (S53 R1, R6): KHR_texture_basisu's KTX2 through the Basis
    // transcoder, EXT_texture_webp's WebP through the editor's decoder, both to RGBA8
    // a WebP image in a game: GltfNormalize reported its texture (the cook transcodes it)
    if (GltfNormalize::isWebp(encoded.data(), encoded.size()) && !GltfNormalize::hasWebpDecoder()) {
        return;
    }
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    if (GltfNormalize::decodeImage(encoded.data(), encoded.size(), pixels, width, height, err)) {
        image.image = std::move(pixels);
        image.width = width;
        image.height = height;
        image.component = 4;
        image.bits = 8;
        image.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    } else if (!err.empty()) {
        Log::error("Failed to decode GLTF image %zu: %s", index, err.c_str());
        return;
    } else if (!tinygltf::LoadImageData(&image, static_cast<int>(index), &err, &warn, 0, 0,
                                        encoded.data(), static_cast<int>(encoded.size()), nullptr)) {
        // tinygltf's own decoder (default RGBA8/16), so results match the serial path
        Log::error("Failed to decode GLTF image %zu: %s", index, err.c_str());
        return;
    }

    // Downscale oversized maps for preview renders. Only 8-bit images are handled; 16-bit is rare
    // and left full-size. The transient full-size buffer is freed when this returns.
    if (maxDimension > 0 && image.bits == 8 && image.component >= 1 &&
            (image.width > maxDimension || image.height > maxDimension)) {
        const int comp = image.component;
        const double scale = static_cast<double>(maxDimension) / std::max(image.width, image.height);
        const int nw = std::max(1, static_cast<int>(std::lround(image.width * scale)));
        const int nh = std::max(1, static_cast<int>(std::lround(image.height * scale)));
        std::vector<unsigned char> resized(static_cast<size_t>(nw) * nh * comp);
        const stbir_pixel_layout layout = (comp == 4) ? STBIR_RGBA : (comp == 3) ? STBIR_RGB :
                                          (comp == 2) ? STBIR_RA : STBIR_1CHANNEL;
        if (stbir_resize_uint8_linear(image.image.data(), image.width, image.height, 0,
                                      resized.data(), nw, nh, 0, layout)) {
            image.image = std::move(resized);
            image.width = nw;
            image.height = nh;
        }
    }
}

// glTF embedded images are decoded serially inside LoadBinaryFromFile, which dominates load time
// for texture-heavy models. Instead the loader is put in "as is" mode (it stores the still-encoded
// bytes), and this decodes them across a few threads afterwards.
void MeshSystem::decodeGLTFImagesParallel(tinygltf::Model& model, int maxDimension) {
    // legal inside the Background job parsing the model: the forks stay Background
    jobs::parallelFor(model.images.size(), [&model, maxDimension](size_t i) {
        decodeGLTFImage(model.images[i], i, maxDimension);
    });
}

// Copies one decoded glTF texture's pixels into a pooled face array (the malloc + memcpy that
// otherwise stalls the main thread). Mirrors the ownership dance in loadGLTFTexture: build the
// TextureData with dataOwned=false, then mark the stored copy owned so there is a single owner.
std::shared_ptr<std::array<TextureData,6>> MeshSystem::buildGLTFTextureFaces(tinygltf::Model& model, int textureIndex) {
    if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= model.textures.size()) {
        return nullptr;
    }
    const tinygltf::Texture& tex = model.textures[textureIndex];
    if (tex.source < 0 || static_cast<size_t>(tex.source) >= model.images.size()) {
        return nullptr;
    }
    const tinygltf::Image& image = model.images[tex.source];
    if (image.width <= 0 || image.height <= 0 || image.component <= 0 || image.image.empty()) {
        return nullptr;
    }
    const size_t imageSize = static_cast<size_t>(image.component) * image.width * image.height;
    if (image.image.size() < imageSize) {
        return nullptr;
    }
    ColorFormat colorFormat;
    if (image.component == 1) {
        colorFormat = ColorFormat::RED;
    } else if (image.component == 4) {
        colorFormat = ColorFormat::RGBA;
    } else {
        return nullptr; // renders only support 8bpp and 32bpp
    }

    unsigned char* imageCopy = static_cast<unsigned char*>(std::malloc(imageSize));
    if (!imageCopy) {
        return nullptr;
    }
    std::memcpy(imageCopy, image.image.data(), imageSize);

    auto faces = std::make_shared<std::array<TextureData,6>>();
    faces->at(0) = TextureData(image.width, image.height, static_cast<unsigned int>(imageSize), colorFormat, image.component, imageCopy);
    faces->at(0).setDataOwned(true); // single owner; copies above are shallow with dataOwned=false
    return faces;
}

std::shared_ptr<MeshSystem::AsyncModelLoadResult> MeshSystem::loadModelFileOnWorker(const std::string& filename, bool obj, uint64_t buildId, std::vector<uint8_t>& bytes){
    auto result = std::make_shared<AsyncModelLoadResult>();

    try {
        ResourceProgress::updateProgress(buildId, 0.1f);

        if (obj){
            tinyobj::FileReader::externalFunc = readFileToString;
            std::string baseDir = FileData::getBaseDir(filename);
            result->objModel = std::make_shared<ObjModelData>();
            // the .obj from the bytes read; its .mtl through the same file callbacks as a sync load
            std::istringstream objStream(std::string(bytes.begin(), bytes.end()));
            tinyobj::MaterialFileReader materialReader(baseDir);
            result->success = tinyobj::LoadObj(&result->objModel->attrib, &result->objModel->shapes, &result->objModel->materials, &result->warn, &result->err, &objStream, &materialReader);
        }else{
            result->gltfModel = std::make_shared<tinygltf::Model>();

            tinygltf::TinyGLTF loader;
            loader.SetFsCallbacks({&fileExists, &tinygltf::ExpandFilePath, &readWholeFile, &tinygltf::WriteWholeFile, &getFileSizeInBytes});
            loader.SetImagesAsIs(true); // keep images encoded; decode in parallel below

            // the main file from the bytes read; buffers and images it names through the
            // file callbacks, relative to its directory (as LoadBinaryFromFile does). An FBX
            // loads from its imported .glb and a cooked model from its cooked .glb: the bytes
            // read are that file's (getImportedModelPath), binary or not by their first bytes.
            const std::string baseDir = FileData::getBaseDir(filename);
            if (isGlbData(bytes.data(), bytes.size())) {
                result->success = loader.LoadBinaryFromMemory(result->gltfModel.get(), &result->err, &result->warn, bytes.data(), (unsigned int)bytes.size(), baseDir);
            }else{
                result->success = loader.LoadASCIIFromString(result->gltfModel.get(), &result->err, &result->warn, reinterpret_cast<const char*>(bytes.data()), (unsigned int)bytes.size(), baseDir);
            }
            if (result->success && result->err.empty()) {
                result->success = normalizeGLTF(*result->gltfModel, filename, &result->err);
            }
            if (result->success) {
                decodeGLTFImagesParallel(*result->gltfModel, 0); // background loads are full resolution

                // Pre-copy texture pixels on the worker thread so the main-thread build only
                // does pool lookups, keyed to match loadGLTFTexture.
                const std::string modelKey = getModelTextureKey(filename);
                const size_t textureCount = result->gltfModel->textures.size();

                // One copy per dedup key, not per image source: each key is a separate
                // TexturePool id needing its own upload, and the pixels are freed after it.
                std::vector<std::string> texKeys(textureCount);
                std::unordered_map<std::string, std::shared_ptr<std::array<TextureData,6>>> facesByKey;
                std::vector<int> keyReps; // texture index building each unique key
                for (size_t i = 0; i < textureCount; i++) {
                    if (result->gltfModel->textures[i].source < 0) continue;
                    texKeys[i] = gltfTextureDedupKey(modelKey, *result->gltfModel, static_cast<int>(i));
                    if (facesByKey.emplace(texKeys[i], nullptr).second) {
                        keyReps.push_back(static_cast<int>(i));
                    }
                }

                // Build into a plain vector: workers touch only their own slot, never the map.
                std::vector<std::shared_ptr<std::array<TextureData,6>>> builtFaces(keyReps.size());
                jobs::parallelFor(keyReps.size(), [&](size_t j) {
                    builtFaces[j] = buildGLTFTextureFaces(*result->gltfModel, keyReps[j]);
                });
                for (size_t j = 0; j < keyReps.size(); j++) {
                    facesByKey[texKeys[keyReps[j]]] = std::move(builtFaces[j]);
                }

                result->prebuiltTextures.resize(textureCount);
                for (size_t i = 0; i < textureCount; i++) {
                    if (texKeys[i].empty()) continue;
                    auto it = facesByKey.find(texKeys[i]);
                    if (it != facesByKey.end() && it->second) {
                        result->prebuiltTextures[i] = { texKeys[i], it->second };
                    }
                }
            }
        }

        if (!result->err.empty() || !result->success){
            ResourceProgress::failBuild(buildId);
            return result;
        }

        ResourceProgress::updateProgress(buildId, 0.3f);
    } catch (const std::exception& e) {
        result->success = false;
        result->err = e.what();
        ResourceProgress::failBuild(buildId);
    }

    return result;
}

namespace {
    std::atomic<int> modelLoadTestDelayMs{0};
}

void MeshSystem::setModelLoadDelayForTest(int milliseconds){
    modelLoadTestDelayMs.store(std::max(0, milliseconds));
}

std::shared_ptr<MeshSystem::AsyncModelLoadResult> MeshSystem::pollOrStartAsyncModelLoad(const Scene* scene, const std::string& filename, bool obj){
    const std::string key = getAsyncModelLoadKey(scene, filename);
    const uint64_t buildId = std::hash<std::string>{}(key);

    jobs::Task<std::shared_ptr<AsyncModelLoadResult>> task;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        auto it = pendingModelLoads.find(key);
        if (it == pendingModelLoads.end()){
            std::filesystem::path filePath(filename);
            ResourceProgress::startBuild(buildId, ResourceType::Model, filePath.filename().string());

            // the file on an asset reader thread, the parse as a Background job
            AssetIOService::ReadRequest request;
            request.path = obj ? filename : getImportedModelPath(filename);
            request.priority = IOPriority::Normal;
            request.name = obj ? "obj-parse" : "gltf-parse";
            request.onFailure = [buildId](const std::string&) {
                ResourceProgress::failBuild(buildId);
            };
            pendingModelLoads[key] = AssetIOService::instance().submitRead(request,
                [filename, obj, buildId](std::vector<uint8_t>&& bytes) {
                    if (const int delay = modelLoadTestDelayMs.load(); delay > 0){
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    }
                    return MeshSystem::loadModelFileOnWorker(filename, obj, buildId, bytes);
                }
            );

            return nullptr;
        }

        task = it->second;
    }

    if (!task.isReady()){
        return nullptr;
    }

    std::shared_ptr<AsyncModelLoadResult> result;
    try {
        result = task.get();
    } catch (const std::exception& e) {
        result = std::make_shared<AsyncModelLoadResult>();
        result->success = false;
        result->err = e.what();
    }
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        pendingModelLoads.erase(key);
    }

    if (!result){
        ResourceProgress::failBuild(buildId);
        return nullptr;
    }

    if (!obj && result->gltfModel && !result->warn.empty())
        result->warn = withoutCookedImageWarnings(result->warn, *result->gltfModel, filename);
    if (!result->warn.empty()){
        if (obj){
            Log::warn("Loading OBJ model (%s): %s", filename.c_str(), result->warn.c_str());
        }else{
            Log::warn("Loading GLTF model (%s): %s", filename.c_str(), result->warn.c_str());
        }
    }

    if (!result->err.empty()){
        if (obj){
            Log::error("Can't load OBJ model (%s): %s", filename.c_str(), result->err.c_str());
        }else{
            Log::error("Can't load GLTF model (%s): %s", filename.c_str(), result->err.c_str());
        }
    }

    if (!result->success && !obj && result->err.empty()){
        Log::verbose("Failed to load glTF: %s", filename.c_str());
    }

    return result;
}

struct MeshSystem::ModelReload {
    std::string filename;
    std::string poolKey;
    bool obj = false;
    std::shared_ptr<AsyncModelLoadResult> result;
};

std::shared_ptr<MeshSystem::ModelReload> MeshSystem::parseModelForReload(const std::string& filename){
    auto reload = std::make_shared<ModelReload>();
    reload->filename = filename;
    reload->poolKey = getModelFilenameKey(filename);
    reload->obj = FileData::getFilePathExtension(filename) == "obj";
    // the whole file (the I/O service's read, with its retry window for a file being replaced)
    std::vector<uint8_t> bytes;
    std::string readError;
    if (!AssetIOService::readFile(reload->obj ? filename : getImportedModelPath(filename), 0, 0, bytes, readError))
        throw std::runtime_error(readError);
    reload->result = loadModelFileOnWorker(filename, reload->obj, 0, bytes);
    if (!reload->result || !reload->result->success){
        const std::string error = reload->result && !reload->result->err.empty() ? reload->result->err : std::string("the model could not be parsed");
        throw std::runtime_error(error);
    }
    return reload;
}

bool MeshSystem::modelHierarchyMatches(const ModelReload& reload, std::string& why){
    if (reload.obj){
        std::shared_ptr<ObjModelData> loaded = ModelPool::getObj(reload.poolKey);
        if (!loaded || !reload.result->objModel){
            return true;
        }
        const auto& before = loaded->shapes;
        const auto& after = reload.result->objModel->shapes;
        if (before.size() != after.size()){
            why = "it has " + std::to_string(after.size()) + " shapes instead of " + std::to_string(before.size());
            return false;
        }
        for (size_t i = 0; i < before.size(); i++){
            if (before[i].name != after[i].name){
                why = "shape " + std::to_string(i) + " is \"" + after[i].name + "\" instead of \"" + before[i].name + "\"";
                return false;
            }
        }
        return true;
    }
    std::shared_ptr<tinygltf::Model> loaded = ModelPool::getGLTF(reload.poolKey);
    if (!loaded || !reload.result->gltfModel){
        return true;
    }
    const auto& before = loaded->nodes;
    const auto& after = reload.result->gltfModel->nodes;
    if (before.size() != after.size()){
        why = "it has " + std::to_string(after.size()) + " nodes instead of " + std::to_string(before.size());
        return false;
    }
    for (size_t i = 0; i < before.size(); i++){
        if (before[i].name != after[i].name || before[i].children != after[i].children || (before[i].mesh >= 0) != (after[i].mesh >= 0)){
            why = "node " + std::to_string(i) + " (\"" + after[i].name + "\") is not the one loaded (\"" + before[i].name + "\")";
            return false;
        }
    }
    return true;
}

void MeshSystem::adoptModelReload(const std::shared_ptr<ModelReload>& reload){
    if (!reload || !reload->result){
        return;
    }
    if (reload->obj){
        ModelPool::replaceObj(reload->poolKey, reload->result->objModel);
    }else{
        ModelPool::replaceGLTF(reload->poolKey, reload->result->gltfModel);
        // the embedded textures are new pixels too: their Textures take them (generation)
        for (auto& prebuilt : reload->result->prebuiltTextures){
            if (prebuilt.second){
                TextureDataPool::replace(prebuilt.first, prebuilt.second);
                TexturePool::invalidate(prebuilt.first);
            }
        }
    }
}

bool MeshSystem::rebuildModelFile(const std::string& filename){
    const std::string key = getModelFilenameKey(filename);
    std::vector<Entity> entities;
    auto models = scene->getComponentArray<ModelComponent>();
    for (int i = 0; i < models->size(); i++){
        const ModelComponent& model = models->getComponentFromIndex(i);
        if (!model.filename.empty() && getModelFilenameKey(model.filename) == key){
            entities.push_back(models->getEntity(i));
        }
    }
    // destroying entities compacts the component arrays: by entity, one at a time
    for (Entity entity : entities){
        ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        if (!model){
            continue;
        }
        clearMeshNodeMapping(*model);
        for (Entity animation : model->animations){
            if (scene->isEntityCreated(animation)){
                scene->destroyEntity(animation);
            }
        }
        model = scene->findComponent<ModelComponent>(entity);
        if (model){
            model->animations.clear();
        }
    }
    return reloadModelFile(filename);
}

bool MeshSystem::reloadModelFile(const std::string& filename){
    const std::string key = getModelFilenameKey(filename);
    bool any = false;
    auto models = scene->getComponentArray<ModelComponent>();
    for (int i = 0; i < models->size(); i++){
        ModelComponent& model = models->getComponentFromIndex(i);
        if (model.filename.empty() || getModelFilenameKey(model.filename) != key){
            continue;
        }
        // the cached parse is the new one: createOrUpdateModel loads it on the GameObjects it has
        model.loadedFilename.clear();
        model.needUpdateModel = true;
        failedModelLoads.erase(model.filename);
        any = true;
    }
    return any;
}

bool MeshSystem::preloadModel(const std::string& filename, std::shared_ptr<void>& data){
    // parsing on this thread would stall the game
    if (!Engine::isAsyncLoading()){
        return true;
    }

    const std::string poolKey = getModelFilenameKey(filename);
    const bool obj = FileData::getFilePathExtension(filename) == "obj";

    if (obj){
        if (auto cached = ModelPool::getObj(poolKey)){
            data = cached;
            return true;
        }
    }else if (auto cached = ModelPool::getGLTF(poolKey)){
        data = cached;
        return true;
    }

    std::shared_ptr<AsyncModelLoadResult> result = pollOrStartAsyncModelLoad(nullptr, filename, obj);
    if (!result){
        // no result is a load still running, unless it could not start
        return !isModelPreloading(filename);
    }

    if (!result->success){
        return true;
    }

    // the worker leaves the build open for the scene that uses the model
    ResourceProgress::completeBuild(std::hash<std::string>{}(getAsyncModelLoadKey(nullptr, filename)));

    if (obj){
        data = ModelPool::addObj(poolKey, result->objModel);
    }else{
        data = ModelPool::addGLTF(poolKey, result->gltfModel);
        // as loadGLTF does, so the scene finds the textures pooled
        for (auto& prebuilt : result->prebuiltTextures){
            if (prebuilt.second){
                TextureDataPool::put(prebuilt.first, prebuilt.second);
            }
        }
    }

    return true;
}

void MeshSystem::cancelPreloadModel(const std::string& filename){
    cancelAsyncModelLoadByKey(getAsyncModelLoadKey(nullptr, filename));
}

// The texture ids of parses invalidateModelParse dropped, by pool key: the images stay (entities
// show them), invalidateModelFile still finds them without the parse
static std::mutex& getDroppedParseTexturesMutex(){
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

static std::map<std::string, std::set<std::string>>& getDroppedParseTextures(){
    static std::map<std::string, std::set<std::string>>* textures = new std::map<std::string, std::set<std::string>>();
    return *textures;
}

void MeshSystem::invalidateModelFile(const std::string& filename){
    const std::string poolKey = getModelFilenameKey(filename);
    // the pooled images are keyed by the model's texture key (asset-relative), as loads key them
    const std::string textureKey = getModelTextureKey(filename);
    std::set<std::string> textureIds;
    {
        std::lock_guard<std::mutex> lock(getDroppedParseTexturesMutex());
        auto dropped = getDroppedParseTextures().find(poolKey);
        if (dropped != getDroppedParseTextures().end()){
            textureIds = std::move(dropped->second);
            getDroppedParseTextures().erase(dropped);
        }
    }
    std::shared_ptr<tinygltf::Model> model = ModelPool::getGLTF(poolKey);
    if (model){
        for (size_t i = 0; i < model->textures.size(); i++){
            if (model->textures[i].source >= 0){
                textureIds.insert(gltfTextureDedupKey(textureKey, *model, static_cast<int>(i)));
            }
        }
    }
    for (const std::string& id : textureIds){
        TextureDataPool::invalidate(id);
        TexturePool::invalidate(id);
    }
    ModelPool::invalidate(poolKey);
    // the sub-clips built from the old parse (spec 9.R13)
    AnimationClipPool::invalidateModel(filename);
}

void MeshSystem::invalidateModelParse(const std::string& filename){
    const std::string poolKey = getModelFilenameKey(filename);
    // the pooled images are keyed by the model's texture key (asset-relative), as loads key them
    const std::string textureKey = getModelTextureKey(filename);
    if (std::shared_ptr<tinygltf::Model> model = ModelPool::getGLTF(poolKey)){
        std::lock_guard<std::mutex> lock(getDroppedParseTexturesMutex());
        for (size_t i = 0; i < model->textures.size(); i++){
            if (model->textures[i].source >= 0){
                getDroppedParseTextures()[poolKey].insert(gltfTextureDedupKey(textureKey, *model, static_cast<int>(i)));
            }
        }
    }
    ModelPool::invalidate(poolKey);
    AnimationClipPool::invalidateModel(filename);
}

void MeshSystem::releasePreloadedModel(const std::string& filename){
    const std::string poolKey = getModelFilenameKey(filename);
    // the pooled images are keyed by the model's texture key (asset-relative), as loads key them
    const std::string textureKey = getModelTextureKey(filename);
    if (FileData::getFilePathExtension(filename) == "obj"){
        ModelPool::removeObj(poolKey);
        return;
    }

    std::shared_ptr<tinygltf::Model> model = ModelPool::getGLTF(poolKey);
    if (!model){
        return;
    }

    std::vector<std::string> textureIds;
    for (size_t i = 0; i < model->textures.size(); i++){
        if (model->textures[i].source >= 0){
            textureIds.push_back(gltfTextureDedupKey(textureKey, *model, static_cast<int>(i)));
        }
    }

    model.reset();
    ModelPool::removeGLTF(poolKey);
    // a scene using the model keeps its images too
    if (ModelPool::getGLTF(poolKey)){
        return;
    }

    for (const std::string& id : textureIds){
        TextureDataPool::remove(id);
    }
}

void MeshSystem::addSubmeshAttribute(Submesh& submesh, const std::string& bufferName, AttributeType attribute, unsigned int elements, AttributeDataType dataType, size_t size, size_t offset, bool normalized){
    Attribute attData;

    attData.setBufferName(bufferName);
    attData.setDataType(dataType);
    attData.setElements(elements);
    attData.setCount(size);
    attData.setOffset(offset);
    attData.setNormalized(normalized);

    submesh.attributes[attribute] = attData;
}

bool MeshSystem::loadGLTFBuffer(int bufferViewIndex, MeshComponent& mesh, ModelComponent& model, const int stride, std::vector<std::string>& loadedBuffers){
    if (!model.gltfModel || !isValidGLTFIndex(bufferViewIndex, model.gltfModel->bufferViews)) {
        Log::error("Invalid GLTF buffer view index %i", bufferViewIndex);
        return false;
    }

    const tinygltf::BufferView &bufferView = model.gltfModel->bufferViews[bufferViewIndex];
    if (!isValidGLTFIndex(bufferView.buffer, model.gltfModel->buffers)) {
        Log::error("Invalid GLTF buffer index %i for buffer view %i", bufferView.buffer, bufferViewIndex);
        return false;
    }

    std::vector<unsigned char>& bufferData = model.gltfModel->buffers[bufferView.buffer].data;
    if (bufferView.byteOffset > bufferData.size() || bufferView.byteLength > bufferData.size() - bufferView.byteOffset) {
        Log::error("Invalid GLTF buffer view range %i", bufferViewIndex);
        return false;
    }

    const std::string name = getBufferName(bufferViewIndex, model);

    if (std::find(loadedBuffers.begin(), loadedBuffers.end(), name) == loadedBuffers.end() && bufferView.target != 0) {
        loadedBuffers.push_back(name);

        if (bufferView.target == 34962) { //GL_ARRAY_BUFFER
            mesh.eBuffers[mesh.numExternalBuffers].setType(BufferType::VERTEX_BUFFER);
        } else if (bufferView.target == 34963) { //GL_ELEMENT_ARRAY_BUFFER
            mesh.eBuffers[mesh.numExternalBuffers].setType(BufferType::INDEX_BUFFER);
        }

        // the parse's bytes, never written after it (a load only appends converted copies): every
        // mesh showing them, of every Model of the file, draws from one GPU buffer (MeshBufferPool)
        mesh.eBuffers[mesh.numExternalBuffers].setModelData(bufferData.data() + bufferView.byteOffset, bufferView.byteLength,
                                                            std::static_pointer_cast<const void>(model.gltfModel), bufferView.buffer, bufferView.byteOffset);
        mesh.eBuffers[mesh.numExternalBuffers].setStride(stride);
        mesh.eBuffers[mesh.numExternalBuffers].setName(name);
        mesh.eBuffers[mesh.numExternalBuffers].setRenderAttributes(false);

        mesh.numExternalBuffers++;

        return true;
    }

    return false;
}

int MeshSystem::convertGLTFByteIndicesToShort(const tinygltf::Accessor& indexAccessor, ModelComponent& model){
    // Sokol (and most modern backends) have no 8-bit index format, so 8-bit GLTF indices
    // are expanded to 16-bit into a synthetic buffer/bufferView appended to the model.
    // The model's buffers vector is reserved up-front by the caller so this append never
    // reallocates and invalidates pointers already handed to external buffers.
    if (!model.gltfModel || !isValidGLTFIndex(indexAccessor.bufferView, model.gltfModel->bufferViews)) {
        return -1;
    }

    const tinygltf::BufferView& srcView = model.gltfModel->bufferViews[indexAccessor.bufferView];
    if (!isValidGLTFIndex(srcView.buffer, model.gltfModel->buffers)) {
        return -1;
    }

    const std::vector<unsigned char>& srcData = model.gltfModel->buffers[srcView.buffer].data;
    const size_t srcOffset = srcView.byteOffset + indexAccessor.byteOffset;
    const size_t count = indexAccessor.count;

    // 8-bit indices are tightly packed (one byte each).
    if (srcOffset > srcData.size() || count > srcData.size() - srcOffset) {
        Log::error("GLTF byte index range out of bounds");
        return -1;
    }

    tinygltf::Buffer newBuffer;
    newBuffer.data.resize(count * sizeof(uint16_t));
    uint16_t* dst = reinterpret_cast<uint16_t*>(newBuffer.data.data());
    for (size_t k = 0; k < count; k++) {
        dst[k] = static_cast<uint16_t>(srcData[srcOffset + k]);
    }
    newBuffer.name = kSyntheticBufferName;
    model.gltfModel->buffers.push_back(std::move(newBuffer));
    const int newBufferIndex = static_cast<int>(model.gltfModel->buffers.size()) - 1;

    tinygltf::BufferView newView;
    newView.buffer = newBufferIndex;
    newView.byteOffset = 0;
    newView.byteLength = count * sizeof(uint16_t);
    newView.byteStride = 0;
    newView.target = 34963; // GL_ELEMENT_ARRAY_BUFFER
    newView.name = "byteindex_synth";
    model.gltfModel->bufferViews.push_back(std::move(newView));

    return static_cast<int>(model.gltfModel->bufferViews.size()) - 1;
}

int MeshSystem::materializeGLTFSparseAccessor(tinygltf::Accessor& accessor, ModelComponent& model){
    // A sparse accessor may omit bufferView entirely; in that case its dense base is all zeroes.
    // GPU vertex buffers cannot consume the sparse index/value representation directly, so expand
    // it once into a tightly packed synthetic buffer and update the cached tinygltf accessor. The
    // updated accessor is then reused by later instances of the same pooled model.
    if (!model.gltfModel || !accessor.sparse.isSparse) {
        return accessor.bufferView;
    }

    const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
    const int componentCount = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
    if (componentSize <= 0 || componentCount <= 0 || accessor.count == 0 ||
            accessor.count > std::numeric_limits<size_t>::max() / static_cast<size_t>(componentSize * componentCount)) {
        Log::warn("Cannot materialize invalid sparse GLTF accessor");
        return -1;
    }

    const size_t elementSize = static_cast<size_t>(componentSize) * static_cast<size_t>(componentCount);
    const size_t denseSize = accessor.count * elementSize;
    tinygltf::Buffer denseBuffer;
    denseBuffer.data.assign(denseSize, 0);

    auto validViewRange = [&](const tinygltf::BufferView& view, size_t relativeOffset, size_t bytes) -> bool {
        if (!isValidGLTFIndex(view.buffer, model.gltfModel->buffers) ||
                relativeOffset > view.byteLength || bytes > view.byteLength - relativeOffset) {
            return false;
        }
        const std::vector<unsigned char>& data = model.gltfModel->buffers[view.buffer].data;
        return view.byteOffset <= data.size() && relativeOffset <= data.size() - view.byteOffset &&
               bytes <= data.size() - view.byteOffset - relativeOffset;
    };

    // Copy the optional dense base while removing any source byte stride.
    if (accessor.bufferView >= 0) {
        if (!isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
            Log::warn("Sparse GLTF accessor has invalid base buffer view %i", accessor.bufferView);
            return -1;
        }
        const tinygltf::BufferView& baseView = model.gltfModel->bufferViews[accessor.bufferView];
        const int baseStride = accessor.ByteStride(baseView);
        if (baseStride < static_cast<int>(elementSize) ||
                accessor.count - 1 > (std::numeric_limits<size_t>::max() - elementSize) / static_cast<size_t>(baseStride)) {
            Log::warn("Sparse GLTF accessor has invalid base stride");
            return -1;
        }
        const size_t baseBytes = (accessor.count - 1) * static_cast<size_t>(baseStride) + elementSize;
        if (!validViewRange(baseView, accessor.byteOffset, baseBytes)) {
            Log::warn("Sparse GLTF accessor base range is out of bounds");
            return -1;
        }
        const std::vector<unsigned char>& baseData = model.gltfModel->buffers[baseView.buffer].data;
        const unsigned char* base = baseData.data() + baseView.byteOffset + accessor.byteOffset;
        for (size_t i = 0; i < accessor.count; i++) {
            std::memcpy(denseBuffer.data.data() + i * elementSize,
                        base + i * static_cast<size_t>(baseStride), elementSize);
        }
    } else if (accessor.bufferView != -1) {
        Log::warn("Sparse GLTF accessor has invalid base buffer view %i", accessor.bufferView);
        return -1;
    }

    const tinygltf::Accessor::Sparse& sparse = accessor.sparse;
    if (sparse.count <= 0 || static_cast<size_t>(sparse.count) > accessor.count ||
            !isValidGLTFIndex(sparse.indices.bufferView, model.gltfModel->bufferViews) ||
            !isValidGLTFIndex(sparse.values.bufferView, model.gltfModel->bufferViews)) {
        Log::warn("Sparse GLTF accessor has invalid sparse metadata");
        return -1;
    }

    size_t indexSize = 0;
    if (sparse.indices.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
        indexSize = sizeof(uint8_t);
    } else if (sparse.indices.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        indexSize = sizeof(uint16_t);
    } else if (sparse.indices.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
        indexSize = sizeof(uint32_t);
    } else {
        Log::warn("Sparse GLTF accessor has invalid index component type %i", sparse.indices.componentType);
        return -1;
    }

    const size_t sparseCount = static_cast<size_t>(sparse.count);
    if (sparseCount > std::numeric_limits<size_t>::max() / elementSize ||
            sparseCount > std::numeric_limits<size_t>::max() / indexSize) {
        Log::warn("Sparse GLTF accessor size overflows");
        return -1;
    }

    const tinygltf::BufferView& indexView = model.gltfModel->bufferViews[sparse.indices.bufferView];
    const tinygltf::BufferView& valueView = model.gltfModel->bufferViews[sparse.values.bufferView];
    if (!validViewRange(indexView, sparse.indices.byteOffset, sparseCount * indexSize) ||
            !validViewRange(valueView, sparse.values.byteOffset, sparseCount * elementSize)) {
        Log::warn("Sparse GLTF accessor index/value range is out of bounds");
        return -1;
    }

    const std::vector<unsigned char>& indexData = model.gltfModel->buffers[indexView.buffer].data;
    const std::vector<unsigned char>& valueData = model.gltfModel->buffers[valueView.buffer].data;
    const unsigned char* indices = indexData.data() + indexView.byteOffset + sparse.indices.byteOffset;
    const unsigned char* values = valueData.data() + valueView.byteOffset + sparse.values.byteOffset;

    for (size_t i = 0; i < sparseCount; i++) {
        uint32_t denseIndex = 0;
        std::memcpy(&denseIndex, indices + i * indexSize, indexSize);
        if (denseIndex >= accessor.count) {
            Log::warn("Sparse GLTF accessor index %u is out of bounds", denseIndex);
            return -1;
        }
        std::memcpy(denseBuffer.data.data() + static_cast<size_t>(denseIndex) * elementSize,
                    values + i * elementSize, elementSize);
    }

    denseBuffer.name = kSyntheticBufferName;
    model.gltfModel->buffers.push_back(std::move(denseBuffer));
    const int denseBufferIndex = static_cast<int>(model.gltfModel->buffers.size()) - 1;

    tinygltf::BufferView denseView;
    denseView.buffer = denseBufferIndex;
    denseView.byteOffset = 0;
    denseView.byteLength = denseSize;
    denseView.byteStride = 0;
    denseView.target = 34962; // GL_ARRAY_BUFFER
    denseView.name = "sparse_accessor_synth";
    model.gltfModel->bufferViews.push_back(std::move(denseView));
    const int denseViewIndex = static_cast<int>(model.gltfModel->bufferViews.size()) - 1;

    accessor.bufferView = denseViewIndex;
    accessor.byteOffset = 0;
    accessor.sparse.isSparse = false;
    accessor.sparse.count = 0;
    return denseViewIndex;
}

int MeshSystem::bakeGLTFTransformedAttribute(const tinygltf::Accessor& accessor, const Matrix4& matrix, const Matrix3& normalMatrix, bool isNormal, ModelComponent& model,
                                             const std::vector<std::pair<int, float>>& morphs){
    // Bakes a node's world transform into a copy of a POSITION/NORMAL attribute, into a synthetic
    // tightly-packed FLOAT VEC3 buffer/view. Used by flattened previews and static merged models.
    // Caller must reserve buffers/bufferViews up-front.
    if (!model.gltfModel || !isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
        return -1;
    }
    // float vec3 positions and normals (and normalized integer normals, to floats) are baked
    if (accessor.type != TINYGLTF_TYPE_VEC3 || (isNormal ? !isBakeableDirection(accessor) : accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)) {
        return -1;
    }

    const tinygltf::BufferView& srcView = model.gltfModel->bufferViews[accessor.bufferView];
    if (!isValidGLTFIndex(srcView.buffer, model.gltfModel->buffers)) {
        return -1;
    }
    const std::vector<unsigned char>& srcData = model.gltfModel->buffers[srcView.buffer].data;

    const size_t count = accessor.count;
    const int stride = accessor.ByteStride(srcView);
    const size_t elementSize = 3 * static_cast<size_t>(tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType)));
    if (count == 0 || stride < static_cast<int>(elementSize)) {
        return -1;
    }
    const size_t base = srcView.byteOffset + accessor.byteOffset;
    if (base + (count - 1) * static_cast<size_t>(stride) + elementSize > srcData.size()) {
        Log::error("GLTF baked attribute range out of bounds");
        return -1;
    }

    // the displacements (float, or normalized bytes and shorts) of one vertex count each
    struct Displacement {
        const unsigned char* data;
        size_t stride;
        int componentType;
        float weight;
    };
    std::vector<Displacement> displacements;
    for (const auto& [index, weight] : morphs) {
        if (!isValidGLTFIndex(index, model.gltfModel->accessors)) {
            continue;
        }
        const tinygltf::Accessor& delta = model.gltfModel->accessors[index];
        const bool readable = delta.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
            (delta.normalized && (delta.componentType == TINYGLTF_COMPONENT_TYPE_BYTE || delta.componentType == TINYGLTF_COMPONENT_TYPE_SHORT));
        if (delta.type != TINYGLTF_TYPE_VEC3 || delta.count != count || !readable ||
                !isValidGLTFIndex(delta.bufferView, model.gltfModel->bufferViews)) {
            Log::warn("GLTF blend shape displacement %i cannot be baked; drawn without it", index);
            continue;
        }
        const tinygltf::BufferView& view = model.gltfModel->bufferViews[delta.bufferView];
        if (!isValidGLTFIndex(view.buffer, model.gltfModel->buffers)) {
            continue;
        }
        const std::vector<unsigned char>& data = model.gltfModel->buffers[view.buffer].data;
        const size_t deltaStride = static_cast<size_t>(delta.ByteStride(view));
        const size_t deltaElement = 3 * static_cast<size_t>(tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(delta.componentType)));
        const size_t deltaBase = view.byteOffset + delta.byteOffset;
        if (deltaStride < deltaElement || deltaBase + (count - 1) * deltaStride + deltaElement > data.size()) {
            Log::error("GLTF blend shape displacement range out of bounds");
            continue;
        }
        displacements.push_back({data.data() + deltaBase, deltaStride, delta.componentType, weight});
    }

    tinygltf::Buffer newBuffer;
    newBuffer.data.resize(count * 3 * sizeof(float));
    float* dst = reinterpret_cast<float*>(newBuffer.data.data());
    for (size_t i = 0; i < count; i++) {
        float src[3];
        readGLTFComponents(srcData.data() + base + i * static_cast<size_t>(stride), accessor.componentType, 3, src);
        Vector3 v(src[0], src[1], src[2]);
        for (const Displacement& displacement : displacements) {
            float d[3];
            readGLTFComponents(displacement.data + i * displacement.stride, displacement.componentType, 3, d);
            v += Vector3(d[0], d[1], d[2]) * displacement.weight;
        }
        Vector3 r = isNormal ? (normalMatrix * v).normalized() : (matrix * v);
        dst[i * 3 + 0] = r.x;
        dst[i * 3 + 1] = r.y;
        dst[i * 3 + 2] = r.z;
    }
    newBuffer.name = kSyntheticBufferName;
    model.gltfModel->buffers.push_back(std::move(newBuffer));
    const int newBufferIndex = static_cast<int>(model.gltfModel->buffers.size()) - 1;

    tinygltf::BufferView newView;
    newView.buffer = newBufferIndex;
    newView.byteOffset = 0;
    newView.byteLength = count * 3 * sizeof(float);
    newView.byteStride = 0; // tightly packed
    newView.target = 34962; // GL_ARRAY_BUFFER
    newView.name = "baked_attr_synth";
    model.gltfModel->bufferViews.push_back(std::move(newView));

    return static_cast<int>(model.gltfModel->bufferViews.size()) - 1;
}

int MeshSystem::bakeGLTFTransformedTangent(const tinygltf::Accessor& accessor, const Matrix3& tangentMatrix, ModelComponent& model){
    // A tangent is a surface direction, so it transforms with the linear matrix. Normals use
    // inverse-transpose instead. This pairing preserves orthogonality under non-uniform scale:
    // dot(M * tangent, inverse(transpose(M)) * normal) == dot(tangent, normal).
    if (!model.gltfModel || !isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
        return -1;
    }
    if (!isBakeableDirection(accessor) || accessor.type != TINYGLTF_TYPE_VEC4) {
        return -1;
    }

    const tinygltf::BufferView& srcView = model.gltfModel->bufferViews[accessor.bufferView];
    if (!isValidGLTFIndex(srcView.buffer, model.gltfModel->buffers)) {
        return -1;
    }
    const std::vector<unsigned char>& srcData = model.gltfModel->buffers[srcView.buffer].data;

    const size_t count = accessor.count;
    const int stride = accessor.ByteStride(srcView);
    const size_t elementSize = 4 * static_cast<size_t>(tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType)));
    if (count == 0 || stride < static_cast<int>(elementSize)) {
        return -1;
    }
    const size_t base = srcView.byteOffset + accessor.byteOffset;
    if (base + (count - 1) * static_cast<size_t>(stride) + elementSize > srcData.size()) {
        Log::error("GLTF baked tangent range out of bounds");
        return -1;
    }

    const float handedness = tangentMatrix.determinant() < 0.0f ? -1.0f : 1.0f;
    tinygltf::Buffer newBuffer;
    newBuffer.data.resize(count * 4 * sizeof(float));
    float* dst = reinterpret_cast<float*>(newBuffer.data.data());
    for (size_t i = 0; i < count; i++) {
        float src[4];
        readGLTFComponents(srcData.data() + base + i * static_cast<size_t>(stride), accessor.componentType, 4, src);
        Vector3 tangent = (tangentMatrix * Vector3(src[0], src[1], src[2])).normalized();
        dst[i * 4 + 0] = tangent.x;
        dst[i * 4 + 1] = tangent.y;
        dst[i * 4 + 2] = tangent.z;
        dst[i * 4 + 3] = src[3] * handedness;
    }
    newBuffer.name = kSyntheticBufferName;
    model.gltfModel->buffers.push_back(std::move(newBuffer));
    const int newBufferIndex = static_cast<int>(model.gltfModel->buffers.size()) - 1;

    tinygltf::BufferView newView;
    newView.buffer = newBufferIndex;
    newView.byteOffset = 0;
    newView.byteLength = count * 4 * sizeof(float);
    newView.byteStride = 0;
    newView.target = 34962; // GL_ARRAY_BUFFER
    newView.name = "baked_tangent_synth";
    model.gltfModel->bufferViews.push_back(std::move(newView));

    return static_cast<int>(model.gltfModel->bufferViews.size()) - 1;
}

int MeshSystem::convertGLTFColorToVec4(const tinygltf::Accessor& accessor, ModelComponent& model){
    // Widens a normalized integer VEC3 COLOR_0 into a synthetic tightly-packed VEC4 of the same
    // component type, with an opaque alpha. Sokol has no UBYTE3N/USHORT3N to bind those directly,
    // while a float VEC3 binds as-is. Caller must reserve buffers/bufferViews up-front.
    if (!model.gltfModel || !isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
        return -1;
    }
    if (accessor.type != TINYGLTF_TYPE_VEC3) {
        return -1;
    }
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
            accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        return -1;
    }
    const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));

    const tinygltf::BufferView& srcView = model.gltfModel->bufferViews[accessor.bufferView];
    if (!isValidGLTFIndex(srcView.buffer, model.gltfModel->buffers)) {
        return -1;
    }
    const std::vector<unsigned char>& srcData = model.gltfModel->buffers[srcView.buffer].data;

    const size_t count = accessor.count;
    const int stride = accessor.ByteStride(srcView);
    if (count == 0 || stride < 3 * componentSize) {
        return -1;
    }
    const size_t base = srcView.byteOffset + accessor.byteOffset;
    if (base + (count - 1) * static_cast<size_t>(stride) + 3 * static_cast<size_t>(componentSize) > srcData.size()) {
        Log::error("GLTF vertex color range out of bounds");
        return -1;
    }

    const size_t colorSize = 3 * static_cast<size_t>(componentSize);
    const size_t elementSize = 4 * static_cast<size_t>(componentSize);
    tinygltf::Buffer newBuffer;
    newBuffer.data.resize(count * elementSize);
    for (size_t i = 0; i < count; i++) {
        unsigned char* dst = newBuffer.data.data() + i * elementSize;
        memcpy(dst, srcData.data() + base + i * static_cast<size_t>(stride), colorSize);
        memset(dst + colorSize, 0xFF, componentSize); // all bits set is 1.0 once normalized
    }
    newBuffer.name = kSyntheticBufferName;
    model.gltfModel->buffers.push_back(std::move(newBuffer));
    const int newBufferIndex = static_cast<int>(model.gltfModel->buffers.size()) - 1;

    tinygltf::BufferView newView;
    newView.buffer = newBufferIndex;
    newView.byteOffset = 0;
    newView.byteLength = count * elementSize;
    newView.byteStride = 0; // tightly packed
    newView.target = 34962; // GL_ARRAY_BUFFER
    newView.name = "color_vec4_synth";
    model.gltfModel->bufferViews.push_back(std::move(newView));

    return static_cast<int>(model.gltfModel->bufferViews.size()) - 1;
}

void MeshSystem::remapMeshNodesByName(ModelComponent& model, const std::vector<int>& meshNodes) {
    std::map<int, Entity> previous;
    for (const auto& node : model.meshNodesMapping) {
        if (scene->findComponent<MeshComponent>(node.second)) {
            previous.insert(node);
        }
    }
    model.meshNodesMapping.clear();

    auto claim = [&](int nodeIdx, std::map<int, Entity>::iterator entry) {
        model.meshNodesMapping[nodeIdx] = entry->second;
        previous.erase(entry);
    };

    // Same index and name first, so an unchanged file keeps every pairing it had
    std::vector<int> unpaired;
    for (int nodeIdx : meshNodes) {
        auto sameIndex = previous.find(nodeIdx);
        if (sameIndex != previous.end() && scene->getEntityName(sameIndex->second) == model.gltfModel->nodes[nodeIdx].name) {
            claim(nodeIdx, sameIndex);
        } else {
            unpaired.push_back(nodeIdx);
        }
    }

    for (int nodeIdx : unpaired) {
        const std::string& nodeName = model.gltfModel->nodes[nodeIdx].name;
        auto match = previous.end();
        if (!nodeName.empty()) {
            match = std::find_if(previous.begin(), previous.end(), [&](const auto& entry) {
                return scene->getEntityName(entry.second) == nodeName;
            });
        }
        // Nodes without a name, and parts the user renamed, have only the index left
        if (match == previous.end()) {
            match = previous.find(nodeIdx);
        }
        if (match != previous.end()) {
            claim(nodeIdx, match);
        }
    }
}

bool MeshSystem::canEditModelHierarchy(const ModelComponent& model, std::string* reason) const {
    auto reject = [reason](const char* message) {
        if (reason) *reason = message;
        return false;
    };

    if (!model.gltfModel || model.needUpdateModel) {
        return reject("Model data is not loaded yet");
    }
    if (model.mergeStaticMeshes || model.meshNodesMapping.empty()) {
        return reject("The model does not have separate mesh parts");
    }

    if (reason) reason->clear();
    return true;
}

bool MeshSystem::canEditModelPart(const ModelComponent& model, Entity part, std::string* reason) const {
    auto reject = [reason](const char* message) {
        if (reason) *reason = message;
        return false;
    };
    if (reason) reason->clear();

    if (scene->findComponent<BoneComponent>(part)) {
        return reject("Joints cannot be reparented");
    }
    for (const auto& bone : model.bonesIdMapping) {
        if (scene->isParentOf(part, bone.second)) {
            return reject("Parts holding joints cannot be reparented");
        }
    }
    if (!model.gltfModel) {
        return true;
    }

    const std::vector<tinygltf::Node>& nodes = model.gltfModel->nodes;
    std::vector<bool> animated(nodes.size(), false);
    for (const auto& animation : model.gltfModel->animations) {
        for (const auto& channel : animation.channels) {
            if (channel.target_path != "weights" && isValidGLTFIndex(channel.target_node, nodes)) {
                animated[channel.target_node] = true;
            }
        }
    }
    std::vector<int> parents(nodes.size(), -1);
    for (size_t i = 0; i < nodes.size(); i++) {
        for (int child : nodes[i].children) {
            if (isValidGLTFIndex(child, nodes)) {
                parents[child] = static_cast<int>(i);
            }
        }
    }
    // Keyframes are local to the glTF parent, so the file's ancestry is what matters, not
    // where the user put the part
    auto inheritsAnimation = [&](int nodeIdx) {
        for (size_t depth = 0; depth < nodes.size() && parents[nodeIdx] >= 0; depth++) {
            nodeIdx = parents[nodeIdx];
            if (animated[nodeIdx]) {
                return true;
            }
        }
        return false;
    };

    for (const auto& meshNode : model.meshNodesMapping) {
        if (!isValidGLTFIndex(meshNode.first, nodes)) {
            continue;
        }
        if (meshNode.second == part && animated[meshNode.first]) {
            return reject("Animated parts cannot be reparented");
        }
        // Skinning ignores the mesh transform, but a rigid mesh would lose the animation it inherits
        if (nodes[meshNode.first].skin < 0 && (meshNode.second == part || scene->isParentOf(part, meshNode.second))
                && inheritsAnimation(meshNode.first)) {
            return reject("Parts inheriting animation cannot be reparented");
        }
    }

    return true;
}

std::string MeshSystem::getModelNodeName(const ModelComponent& model, int nodeIdx) {
    if (!model.gltfModel || !isValidGLTFIndex(nodeIdx, model.gltfModel->nodes)) {
        return "";
    }
    return model.gltfModel->nodes[nodeIdx].name;
}

std::map<int, Entity> MeshSystem::getModelNodeDefaultParents(Entity modelEntity, const ModelComponent& model) const {
    std::map<int, Entity> parents;
    for (const auto& node : model.meshNodesMapping) {
        parents[node.first] = modelEntity;
    }
    for (const auto& node : model.nodesIdMapping) {
        parents[node.first] = modelEntity;
    }
    if (model.gltfModel) {
        for (const auto& node : model.nodesIdMapping) {
            if (isValidGLTFIndex(node.first, model.gltfModel->nodes)) {
                for (int child : model.gltfModel->nodes[node.first].children) {
                    parents[child] = node.second;
                }
            }
        }
    }
    return parents;
}

bool MeshSystem::hasCustomMeshParenting(Entity modelEntity, const ModelComponent& model) const {
    std::map<int, Entity> defaults = getModelNodeDefaultParents(modelEntity, model);
    for (const auto& node : model.meshNodesMapping) {
        Transform* transform = scene->findComponent<Transform>(node.second);
        auto defaultParent = defaults.find(node.first);
        if (transform && defaultParent != defaults.end() && transform->parent != defaultParent->second) {
            return true;
        }
    }
    return false;
}

bool MeshSystem::canBakeGLTFMesh(const ModelComponent& model, int meshIndex, std::string* reason, bool bakesMorphs) const {
    auto reject = [reason](const char* message) {
        if (reason) *reason = message;
        return false;
    };

    if (!model.gltfModel || !isValidGLTFIndex(meshIndex, model.gltfModel->meshes)) {
        return reject("The model contains an invalid mesh reference");
    }

    for (const tinygltf::Primitive& primitive : model.gltfModel->meshes[meshIndex].primitives) {
        if (!primitive.targets.empty() && !bakesMorphs) {
            return reject("Models with morph targets cannot be merged as static geometry");
        }
        for (const auto& attribute : primitive.attributes) {
            if (attribute.first != "POSITION" && attribute.first != "NORMAL" && attribute.first != "TANGENT") {
                continue;
            }
            if (!isValidGLTFIndex(attribute.second, model.gltfModel->accessors)) {
                return reject("The model contains an invalid vertex attribute");
            }
            const tinygltf::Accessor& accessor = model.gltfModel->accessors[attribute.second];
            const int expectedType = attribute.first == "TANGENT" ? TINYGLTF_TYPE_VEC4 : TINYGLTF_TYPE_VEC3;
            const bool bakeable = attribute.first == "POSITION" ? accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT
                                                                : isBakeableDirection(accessor);
            if (!bakeable || accessor.type != expectedType) {
                return reject("A position, normal, or tangent attribute cannot be transformed safely");
            }
        }
    }

    return true;
}

bool MeshSystem::canMergeStaticModel(const ModelComponent& model, std::string* reason) const {
    auto reject = [reason](const char* message) {
        if (reason) *reason = message;
        return false;
    };

    if (!model.gltfModel) {
        return reject("Model data is not loaded yet");
    }

    size_t meshNodeCount = 0;
    size_t primitiveCount = 0;
    for (const tinygltf::Node& node : model.gltfModel->nodes) {
        if (node.mesh < 0) continue;
        meshNodeCount++;
        if (node.skin >= 0) {
            return reject("Skinned models cannot be merged as static geometry");
        }
        if (node.extensions.count("EXT_mesh_gpu_instancing")) {
            return reject("Models with GPU-instanced nodes (EXT_mesh_gpu_instancing) cannot be merged as static geometry");
        }
        if (!canBakeGLTFMesh(model, node.mesh, reason)) {
            return false;
        }
        primitiveCount += model.gltfModel->meshes[node.mesh].primitives.size();
    }
    if (meshNodeCount < 2) {
        return reject("The model does not have multiple mesh nodes to merge");
    }
    if (primitiveCount == 0) {
        return reject("The model has no mesh primitives to merge");
    }
    if (!model.gltfModel->animations.empty()) {
        return reject("Animated models cannot be merged as static geometry");
    }

    if (reason) reason->clear();
    return true;
}

bool MeshSystem::loadGLTFTexture(int textureIndex, ModelComponent& model, Texture& texture, const std::string& textureName){
    texture = Texture();

    if (textureIndex >= 0){
        if (!model.gltfModel || !isValidGLTFIndex(textureIndex, model.gltfModel->textures)) {
            Log::warn("Invalid GLTF texture index %i: %s", textureIndex, textureName.c_str());
            return true;
        }

        tinygltf::Texture &tex = model.gltfModel->textures[textureIndex];
        if (!isValidGLTFIndex(tex.source, model.gltfModel->images)) {
            // a KTX2 or WebP source this build does not decode was reported by GltfNormalize
            const bool reported = tex.source < 0 && (tex.extensions.count("KHR_texture_basisu") ||
                tex.extensions.count("EXT_texture_webp") || tex.extensions.count("KHR_texture_webp"));
            if (!reported) {
                Log::warn("Invalid GLTF image index %i for texture %i: %s", tex.source, textureIndex, textureName.c_str());
            }
            return true;
        }

        auto applySampler = [&](){
            if (tex.sampler >= 0 && isValidGLTFIndex(tex.sampler, model.gltfModel->samplers)){
                tinygltf::Sampler &sampler = model.gltfModel->samplers[tex.sampler];
                texture.setMinFilter(convertFilter(sampler.minFilter));
                texture.setMagFilter(convertFilter(sampler.magFilter));
                texture.setWrapU(convertWrap(sampler.wrapS));
                texture.setWrapV(convertWrap(sampler.wrapT));
            }else if (tex.sampler >= 0){
                Log::warn("Invalid GLTF sampler index %i for texture %i", tex.sampler, textureIndex);
            }
        };

        tinygltf::Image &image = model.gltfModel->images[tex.source];
        // an external image with a cook: loaded from it by path, like any project texture
        const std::string cookedPath = gltfCookedImagePath(model.filename, image);
        if (!cookedPath.empty()){
            texture = Texture(cookedPath);
            applySampler();
            return true;
        }
        if (image.width <= 0 || image.height <= 0 || image.component <= 0 || image.image.empty()) {
            Log::warn("Invalid GLTF image data for texture: %s", textureName.c_str());
            return true;
        }

        size_t imageSize = image.component * image.width * image.height; //in bytes
        if (image.image.size() < imageSize) {
            Log::warn("Incomplete GLTF image data for texture: %s", textureName.c_str());
            return true;
        }

        ColorFormat colorFormat;
        if (image.component == 1){
            colorFormat = ColorFormat::RED;
        }else if (image.component == 4){
            colorFormat = ColorFormat::RGBA;
        }else{
            Log::warn("Not compatible image %s: Renders only support 8bpp and 32bpp", textureName.c_str());
            return true;
        }

        // a preview's images (decoded smaller) are keyed apart from the model's (getImageKeySuffix)
        const std::string imageKeySuffix = getImageKeySuffix();
        std::string id = gltfTextureDedupKey(getModelTextureKey(model.filename), *model.gltfModel, textureIndex) + imageKeySuffix;

        // The pooled CPU copy goes once a GPU texture is made of it (setReleaseDataAfterLoad
        // below), and a Texture of this image that makes its own GPU texture later (another
        // sampler, or one remade after its GPU copy went) found no pixels and drew white. It
        // makes them again from the model's decoded image (ModelPool keeps the model while it is
        // used), as the packed lobe maps do (loadGLTFLobeMap). A full-resolution image whose
        // parse went (a reload replaced it) makes them from the pooled parse of the file.
        std::weak_ptr<tinygltf::Model> source = model.gltfModel;
        const int imageIndex = tex.source;
        const std::string poolKey = imageKeySuffix.empty() ? getModelFilenameKey(model.filename) : std::string();
        TextureDataPool::setGenerator(id, [source, poolKey, imageIndex, colorFormat]() -> TextureData {
            std::shared_ptr<tinygltf::Model> gltf = source.lock();
            if (!gltf && !poolKey.empty())
                gltf = ModelPool::getGLTF(poolKey);
            if (!gltf || imageIndex < 0 || (size_t)imageIndex >= gltf->images.size())
                return TextureData();
            const tinygltf::Image& decoded = gltf->images[imageIndex];
            const size_t size = (size_t)decoded.component * decoded.width * decoded.height;
            if (decoded.width <= 0 || decoded.height <= 0 || decoded.image.size() < size)
                return TextureData();
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(size));
            if (!pixels)
                return TextureData();
            std::memcpy(pixels, decoded.image.data(), size);
            // not owned: the pool's copy owns the pixels (TextureDataPool::loadGenerated)
            return TextureData(decoded.width, decoded.height, (unsigned int)size, colorFormat, decoded.component, pixels);
        });

        if (TextureDataPool::hasPixelsOfSize(id, imageSize)) {
            texture.setId(id);
        } else {
            unsigned char* imageCopy = static_cast<unsigned char*>(std::malloc(imageSize));
            if (!imageCopy) {
                Log::error("Out of memory while copying GLTF texture data for %s", textureName.c_str());
                return false;
            }

            std::memcpy(imageCopy, image.image.data(), imageSize);
            TextureData textureData(image.width, image.height, imageSize, colorFormat, image.component, imageCopy);

            texture.setData(id, textureData);

            TextureData& pooledData = texture.getData();
            if (pooledData.getData() == imageCopy) {
                pooledData.setDataOwned(true);
            } else {
                std::free(imageCopy);
            }
        }

        applySampler();
        // Free the pooled CPU copy after upload; the decoded pixels remain in the
        // ModelPool-retained gltfModel, so loadGLTFTexture re-copies from there on reload.
        texture.setReleaseDataAfterLoad(true);
    }

    return true;
}

std::string MeshSystem::getBufferName(int bufferViewIndex, ModelComponent& model){
    if (!model.gltfModel || !isValidGLTFIndex(bufferViewIndex, model.gltfModel->bufferViews)) {
        return "buffer" + std::to_string(bufferViewIndex);
    }

    const tinygltf::BufferView &bufferView = model.gltfModel->bufferViews[bufferViewIndex];

    // glTF buffer view names are not required to be unique, so keep the view index
    // in the key with a delimiter to avoid accidental name+index collisions.
    if (!bufferView.name.empty())
        return bufferView.name + "#" + std::to_string(bufferViewIndex);
    else
        return "buffer" + std::to_string(bufferViewIndex);

}

Matrix4 MeshSystem::getGLTFNodeMatrix(int nodeIndex, ModelComponent& model){
    if (!model.gltfModel || !isValidGLTFIndex(nodeIndex, model.gltfModel->nodes)) {
        return Matrix4();
    }

    tinygltf::Node node = model.gltfModel->nodes[nodeIndex];

    Matrix4 matrix;

    if (node.matrix.size() == 16){
        matrix = Matrix4(
                node.matrix[0],node.matrix[4],node.matrix[8], node.matrix[12],
                node.matrix[1],node.matrix[5],node.matrix[9], node.matrix[13],
                node.matrix[2],node.matrix[6],node.matrix[10],node.matrix[14],
                node.matrix[3],node.matrix[7],node.matrix[11],node.matrix[15]);
    }else{

        Vector3 translation;
        Vector3 scale;
        Quaternion rotation;

        if (node.translation.size() == 3){
            translation = Vector3(node.translation[0], node.translation[1], node.translation[2]);
        }else{
            translation = Vector3(0.0, 0.0, 0.0);
        }
        if (node.rotation.size() == 4){
            rotation = Quaternion(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
        }else{
            rotation = Quaternion(1.0, 0.0, 0.0, 0.0);
        }
        if (node.scale.size() == 3){
            scale = Vector3(node.scale[0], node.scale[1], node.scale[2]);
        }else{
            scale = Vector3(1.0, 1.0, 1.0);
        }

        Matrix4 scaleMatrix = Matrix4::scaleMatrix(scale);
        Matrix4 translateMatrix = Matrix4::translateMatrix(translation);
        Matrix4 rotationMatrix = rotation.getRotationMatrix();

        matrix = translateMatrix * rotationMatrix * scaleMatrix;
    }

    return matrix;
}

Matrix4 MeshSystem::getGLTFMeshGlobalMatrix(int nodeIndex, ModelComponent& model, std::map<int, int>& nodesParent){

    if (!model.gltfModel || !isValidGLTFIndex(nodeIndex, model.gltfModel->nodes)) {
        return Matrix4();
    }

    Matrix4 matrix = getGLTFNodeMatrix(nodeIndex, model);

    int parent = -1;
    auto parentIt = nodesParent.find(nodeIndex);
    if (parentIt != nodesParent.end()) {
        parent = parentIt->second;
    }

    if (parent >= 0){
        return getGLTFMeshGlobalMatrix(parent, model, nodesParent) * matrix;
    }

    return matrix;
}

Matrix4 MeshSystem::getGLTFInverseBindMatrix(const ModelComponent& model, int skinIndex, size_t jointIndex){
    Matrix4 matrix;
    if (!model.gltfModel || !isValidGLTFIndex(skinIndex, model.gltfModel->skins))
        return matrix;

    const tinygltf::Skin& skin = model.gltfModel->skins[skinIndex];
    if (skin.inverseBindMatrices < 0 ||
            !isValidGLTFIndex(skin.inverseBindMatrices, model.gltfModel->accessors))
        return matrix;

    const tinygltf::Accessor& accessor = model.gltfModel->accessors[skin.inverseBindMatrices];
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.type != TINYGLTF_TYPE_MAT4 ||
            jointIndex >= accessor.count || !isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews))
        return matrix;

    const tinygltf::BufferView& view = model.gltfModel->bufferViews[accessor.bufferView];
    if (!isValidGLTFIndex(view.buffer, model.gltfModel->buffers))
        return matrix;

    const std::vector<unsigned char>& data = model.gltfModel->buffers[view.buffer].data;
    const int byteStride = accessor.ByteStride(view);
    if (byteStride < static_cast<int>(sizeof(float) * 16))
        return matrix;

    const size_t offset = view.byteOffset + accessor.byteOffset + jointIndex * byteStride;
    if (offset > data.size() || sizeof(float) * 16 > data.size() - offset)
        return matrix;

    float values[16];
    std::memcpy(values, data.data() + offset, sizeof(values));
    return Matrix4(
        values[0], values[4], values[8], values[12],
        values[1], values[5], values[9], values[13],
        values[2], values[6], values[10], values[14],
        values[3], values[7], values[11], values[15]);
}

bool MeshSystem::buildGLTFNodeHierarchy(Entity entity, ModelComponent& model,
                                        const std::map<int, int>& nodesParent){
    if (!model.gltfModel)
        return false;

    std::map<int, std::pair<int, size_t>> jointSlots;
    for (size_t skinIndex = 0; skinIndex < model.gltfModel->skins.size(); skinIndex++) {
        const tinygltf::Skin& skin = model.gltfModel->skins[skinIndex];
        for (size_t jointIndex = 0; jointIndex < skin.joints.size(); jointIndex++)
            jointSlots.emplace(skin.joints[jointIndex], std::make_pair(static_cast<int>(skinIndex), jointIndex));
    }

    model.nodesIdMapping.clear();
    model.bonesIdMapping.clear();
    model.bonesNameMapping.clear();
    model.meshNodesMapping.clear();
    model.skeleton = NULL_ENTITY;

    for (size_t i = 0; i < model.gltfModel->nodes.size(); i++) {
        const tinygltf::Node& node = model.gltfModel->nodes[i];
        Entity nodeEntity = scene->createEntity();
        if (nodeEntity == NULL_ENTITY) {
            clearMeshNodeMapping(model);
            return false;
        }

        scene->addComponent<Transform>(nodeEntity);
        Transform& transform = scene->getComponent<Transform>(nodeEntity);
        getGLTFNodeMatrix(static_cast<int>(i), model).decompose(
            transform.position, transform.scale, transform.rotation);
        transform.needUpdate = true;

        scene->setEntityName(nodeEntity,
            node.name.empty() ? "Node " + std::to_string(i) : node.name);
        model.nodesIdMapping[static_cast<int>(i)] = nodeEntity;

        if (node.mesh >= 0) {
            scene->addComponent<MeshComponent>(nodeEntity);
            model.meshNodesMapping[static_cast<int>(i)] = nodeEntity;
        }

        auto joint = jointSlots.find(static_cast<int>(i));
        if (joint != jointSlots.end()) {
            scene->addComponent<BoneComponent>(nodeEntity);
            BoneComponent& bone = scene->getComponent<BoneComponent>(nodeEntity);
            bone.index = static_cast<int>(joint->second.second);
            bone.model = entity;
            bone.bindPosition = transform.position;
            bone.bindRotation = transform.rotation;
            bone.bindScale = transform.scale;
            bone.offsetMatrix = getGLTFInverseBindMatrix(model, joint->second.first, joint->second.second);
            model.bonesIdMapping[static_cast<int>(i)] = nodeEntity;
            model.bonesNameMapping[scene->getEntityName(nodeEntity)] = nodeEntity;
        }
    }

    for (const auto& mappedNode : model.nodesIdMapping) {
        auto parent = nodesParent.find(mappedNode.first);
        Entity parentEntity = entity;
        if (parent != nodesParent.end() && parent->second >= 0) {
            auto mappedParent = model.nodesIdMapping.find(parent->second);
            if (mappedParent != model.nodesIdMapping.end())
                parentEntity = mappedParent->second;
        }
        scene->addEntityChild(parentEntity, mappedNode.second, false);

        if (model.skeleton == NULL_ENTITY && model.bonesIdMapping.count(mappedNode.first) &&
                (parent == nodesParent.end() || !model.bonesIdMapping.count(parent->second)))
            model.skeleton = mappedNode.second;
    }

    return true;
}

void MeshSystem::buildGLTFSkinBindings(Entity entity, ModelComponent& model){
    model.skinBindings.clear();
    if (!model.gltfModel)
        return;

    for (size_t nodeIndex = 0; nodeIndex < model.gltfModel->nodes.size(); nodeIndex++) {
        const tinygltf::Node& node = model.gltfModel->nodes[nodeIndex];
        if (node.mesh < 0 || !isValidGLTFIndex(node.skin, model.gltfModel->skins))
            continue;

        Entity meshEntity = entity;
        auto mappedMesh = model.meshNodesMapping.find(static_cast<int>(nodeIndex));
        if (mappedMesh != model.meshNodesMapping.end())
            meshEntity = mappedMesh->second;

        const tinygltf::Skin& skin = model.gltfModel->skins[node.skin];
        ModelSkinBinding binding;
        binding.mesh = meshEntity;
        binding.joints.reserve(skin.joints.size());
        binding.inverseBindMatrices.reserve(skin.joints.size());

        for (size_t jointIndex = 0; jointIndex < skin.joints.size(); jointIndex++) {
            Entity jointEntity = NULL_ENTITY;
            auto mappedNode = model.nodesIdMapping.find(skin.joints[jointIndex]);
            if (mappedNode != model.nodesIdMapping.end()) {
                jointEntity = mappedNode->second;
            } else {
                auto mappedBone = model.bonesIdMapping.find(skin.joints[jointIndex]);
                if (mappedBone != model.bonesIdMapping.end())
                    jointEntity = mappedBone->second;
            }
            binding.joints.push_back(jointEntity);
            binding.inverseBindMatrices.push_back(getGLTFInverseBindMatrix(model, node.skin, jointIndex));
        }

        model.skinBindings.push_back(std::move(binding));
    }
}

Entity MeshSystem::generateSketetalStructure(Entity entity, ModelComponent& model, int nodeIndex, int skinIndex){
    if (!model.gltfModel || !isValidGLTFIndex(nodeIndex, model.gltfModel->nodes) || !isValidGLTFIndex(skinIndex, model.gltfModel->skins)) {
        return NULL_ENTITY;
    }

    const tinygltf::Node& node = model.gltfModel->nodes[nodeIndex];
    const tinygltf::Skin& skin = model.gltfModel->skins[skinIndex];

    int index = -1;

    for (int j = 0; j < skin.joints.size(); j++) {
        if (nodeIndex == skin.joints[j])
            index = j;
    }

    if (index < 0) {
        return NULL_ENTITY;
    }

    Matrix4 offsetMatrix = getGLTFInverseBindMatrix(model, skinIndex, index);

    Entity bone;

    // User pool (not System): model-generated entities must not draw from the capped [1..1000]
    // System range. They stay out of serialization via the editor's entity list, like mesh nodes.
    bone = scene->createEntity();
    if (bone == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    scene->addComponent<Transform>(bone);
    scene->addComponent<BoneComponent>(bone);

    Transform& bonetransform = scene->getComponent<Transform>(bone);
    BoneComponent& bonecomp = scene->getComponent<BoneComponent>(bone);

    bonecomp.index = index;
    scene->setEntityName(bone, node.name);

    Matrix4 matrix = getGLTFNodeMatrix(nodeIndex, model);
    matrix.decompose(bonecomp.bindPosition, bonecomp.bindScale, bonecomp.bindRotation);

    // move to bind
    bonetransform.position = bonecomp.bindPosition;
    bonetransform.rotation = bonecomp.bindRotation;
    bonetransform.scale = bonecomp.bindScale;

    bonecomp.offsetMatrix = offsetMatrix;
    bonecomp.model = entity;

    model.bonesNameMapping[scene->getEntityName(bone)] = bone;
    model.bonesIdMapping[nodeIndex] = bone;

    for (size_t i = 0; i < node.children.size(); i++){
        // here bonetransform and bonecomp losts references
        if (!isValidGLTFIndex(node.children[i], model.gltfModel->nodes)) {
            continue;
        }

        Entity child = generateSketetalStructure(entity, model, node.children[i], skinIndex);
        if (child != NULL_ENTITY) {
            scene->addEntityChild(bone, child, false);
        }
    }

    return bone;
}

TextureFilter MeshSystem::convertFilter(int filter){
    if (filter==TINYGLTF_TEXTURE_FILTER_NEAREST){
        return TextureFilter::NEAREST;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_LINEAR){
        return TextureFilter::LINEAR;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST){
        return TextureFilter::NEAREST_MIPMAP_NEAREST;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST){
        return TextureFilter::LINEAR_MIPMAP_NEAREST;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR){
        return TextureFilter::NEAREST_MIPMAP_LINEAR;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR){
        return TextureFilter::LINEAR_MIPMAP_LINEAR;
    }

    return TextureFilter::NEAREST;
}

TextureWrap MeshSystem::convertWrap(int wrap){
    if (wrap==TINYGLTF_TEXTURE_WRAP_REPEAT){
        return TextureWrap::REPEAT;
    }else if (wrap==TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE){
        return TextureWrap::CLAMP_TO_EDGE;
    }else if (wrap==TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT){
        return TextureWrap::MIRRORED_REPEAT;
    }

    return TextureWrap::REPEAT;
}

// Bone ids and weights arrive as bytes, shorts or floats and the typed Buffer getters each
// assume a fixed element size. Normalization is undone in the shader, not here.
static bool readAttributeElement(Buffer* buffer, const Attribute& attribute, unsigned int index, unsigned int element, float& out){
    AttributeDataType dataType = attribute.getDataType();

    size_t typeSize = 4;
    if (dataType == AttributeDataType::BYTE || dataType == AttributeDataType::UNSIGNED_BYTE){
        typeSize = 1;
    }else if (dataType == AttributeDataType::SHORT || dataType == AttributeDataType::UNSIGNED_SHORT){
        typeSize = 2;
    }

    size_t pos = ((size_t)index * buffer->getStride()) + attribute.getOffset() + ((size_t)element * typeSize);
    if ((pos + typeSize) > buffer->getSize()){
        return false;
    }

    const unsigned char* data = buffer->getData() + pos;

    if (dataType == AttributeDataType::BYTE){
        int8_t value; memcpy(&value, data, typeSize); out = (float)value;
    }else if (dataType == AttributeDataType::UNSIGNED_BYTE){
        uint8_t value; memcpy(&value, data, typeSize); out = (float)value;
    }else if (dataType == AttributeDataType::SHORT){
        int16_t value; memcpy(&value, data, typeSize); out = (float)value;
    }else if (dataType == AttributeDataType::UNSIGNED_SHORT){
        uint16_t value; memcpy(&value, data, typeSize); out = (float)value;
    }else if (dataType == AttributeDataType::INT){
        int32_t value; memcpy(&value, data, typeSize); out = (float)value;
    }else if (dataType == AttributeDataType::UNSIGNED_INT){
        uint32_t value; memcpy(&value, data, typeSize); out = (float)value;
    }else{
        float value; memcpy(&value, data, typeSize); out = value;
    }

    return true;
}

size_t MeshSystem::skinJointCount(MeshComponent& mesh){
    size_t joints = 0;
    const auto scan = [&joints](Buffer* buffer, const Attribute& ids){
        for (unsigned int v = 0; v < ids.getCount(); v++){
            for (unsigned int e = 0; e < ids.getElements(); e++){
                float id = 0;
                if (readAttributeElement(buffer, ids, v, e, id) && id >= 0.0f)
                    joints = std::max(joints, static_cast<size_t>(id) + 1);
            }
        }
    };
    std::map<std::string, Buffer*> buffers;
    if (mesh.buffer.getSize() > 0)
        buffers["vertices"] = &mesh.buffer;
    for (unsigned int i = 0; i < mesh.numExternalBuffers; i++)
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
    // the ids a buffer lays out itself, and those a submesh names in a buffer
    for (const auto& buffer : buffers){
        if (const Attribute* ids = buffer.second->getAttribute(AttributeType::BONEIDS))
            scan(buffer.second, *ids);
    }
    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
        const auto attribute = mesh.submeshes[s].attributes.find(AttributeType::BONEIDS);
        if (attribute == mesh.submeshes[s].attributes.end())
            continue;
        const auto buffer = buffers.find(attribute->second.getBufferName());
        if (buffer != buffers.end())
            scan(buffer->second, attribute->second);
    }
    return joints;
}

void MeshSystem::calculateMeshAABB(MeshComponent& mesh){
    std::map<std::string, Buffer*> buffers;

    if (mesh.buffer.getSize() > 0){
        buffers["vertices"] = &mesh.buffer;
    }
    for (int i = 0; i < mesh.numExternalBuffers; i++){
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
    }

    Buffer* vertexBuffer = NULL;
    Attribute vertexAttr;

    for (auto const& buf : buffers){
        if (buf.second->getAttribute(AttributeType::POSITION)) {
            vertexBuffer = buf.second;
            vertexAttr = *buf.second->getAttribute(AttributeType::POSITION);
        }
    }

    // Seeded empty, not with AABB::ZERO: that is a finite box at the origin, so merging into it
    // stretched every mesh back to its own origin
    mesh.verticesAABB.setNull();
    mesh.bonesAABB.clear();
    mesh.skinnedAABB.setNull(); // reposed on the next RenderSystem update

    for (size_t i = 0; i < mesh.numSubmeshes; i++) {
        Buffer* boneIdBuffer = NULL;
        Attribute boneIdAttr;
        Buffer* boneWeightBuffer = NULL;
        Attribute boneWeightAttr;

        for (auto const& attr : mesh.submeshes[i].attributes){
            if (attr.first == AttributeType::POSITION){
                vertexBuffer = buffers[attr.second.getBufferName()];
                vertexAttr = attr.second;
            }else if (attr.first == AttributeType::BONEIDS){
                boneIdBuffer = buffers[attr.second.getBufferName()];
                boneIdAttr = attr.second;
            }else if (attr.first == AttributeType::BONEWEIGHTS){
                boneWeightBuffer = buffers[attr.second.getBufferName()];
                boneWeightAttr = attr.second;
            }
        }

        if (!vertexBuffer || vertexAttr.getDataType() != AttributeDataType::FLOAT){
            // cannot create AABB of non float position vertex
            continue;
        }

        // bounds of the vertices each bone influences, posed by RenderSystem
        unsigned int influences = 0;
        if (boneIdBuffer && boneWeightBuffer){
            influences = std::min(boneIdAttr.getElements(), boneWeightAttr.getElements());
        }
        if (influences > 0 && mesh.bonesAABB.empty())
            mesh.bonesAABB.resize(mesh.bonesMatrix.stored());

        int verticesize = int(vertexAttr.getCount());
        for (int v = 0; v < verticesize; v++){
            Vector3 position = vertexBuffer->getVector3(&vertexAttr, v);
            mesh.verticesAABB.merge(position);

            for (unsigned int e = 0; e < influences; e++){
                float weight = 0;
                float boneId = 0;
                if (!readAttributeElement(boneWeightBuffer, boneWeightAttr, v, e, weight)) continue;
                if (weight == 0) continue; // a zero weight never moves the vertex
                if (!readAttributeElement(boneIdBuffer, boneIdAttr, v, e, boneId)) continue;

                int bone = (int)boneId;
                if (bone >= 0 && static_cast<size_t>(bone) < mesh.bonesAABB.size()){
                    mesh.bonesAABB[bone].merge(position);
                }
            }
        }
    }

    // no float positions: AABB::ZERO, the "not computed" value its users test for
    if (mesh.verticesAABB.isNull())
        mesh.verticesAABB = AABB::ZERO;

    mesh.aabb = mesh.verticesAABB;

    mesh.needUpdateAABB = true;
    // picking hashes the new geometry when it next needs it
    mesh.pickGeometryHash = 0;
}

void MeshSystem::createPlaneNodeSubmesh(unsigned int submeshIndex, MeshComponent& mesh, int width, int height, int widthSegments, int heightSegments){
    float width_half = (float)width / 2;
    float height_half = (float)height / 2;

    int gridX = widthSegments;
    int gridY = heightSegments;

    int gridX1 = gridX + 1;
    int gridY1 = gridY + 1;

    float segment_width = (float)width / gridX;
    float segment_height = (float)height / gridY;

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);

    Attribute* attIndice = mesh.indices.getAttribute(AttributeType::INDEX);

    int bufferCount = mesh.buffer.getCount();

    for (int iy = 0; iy < gridY1; iy++) {
        float y = iy * segment_height - height_half;
        for (int ix = 0; ix < gridX1; ix ++) {

            float x = ix * segment_width - width_half;

            mesh.buffer.addVector3(attVertex, Vector3(x, 0, -y));
            mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
        }
    }

    unsigned int bufferIndexCount = 0;
    unsigned int bufferIndexOffset = mesh.indices.getCount();

    for (int iy = 0; iy < gridY; iy++) {
        for (int ix = 0; ix < gridX; ix++) {

            int a = ix + gridX1 * iy;
            int b = ix + gridX1 * ( iy + 1 );
            int c = ( ix + 1 ) + gridX1 * ( iy + 1 );
            int d = ( ix + 1 ) + gridX1 * iy;

            mesh.indices.addUInt16(attIndice, a + bufferCount);
            mesh.indices.addUInt16(attIndice, d + bufferCount);
            mesh.indices.addUInt16(attIndice, b + bufferCount);

            mesh.indices.addUInt16(attIndice, b + bufferCount);
            mesh.indices.addUInt16(attIndice, d + bufferCount);
            mesh.indices.addUInt16(attIndice, c + bufferCount);

            bufferIndexCount += 6;

        }
    }

    addSubmeshAttribute(mesh.submeshes[submeshIndex], "indices", AttributeType::INDEX, 1, AttributeDataType::UNSIGNED_SHORT, bufferIndexCount, bufferIndexOffset * sizeof(uint16_t), false);
}

size_t MeshSystem::getTerrainGridArraySize(int rootGridSize, int levels){
    size_t size = 0;
    for (int i = 0; i<=(levels-1); i++){
        size += pow(4, i);
    }
    size = size * rootGridSize * rootGridSize;

    return size;
}

float MeshSystem::terrainTexelCoord(float local, float terrainSize, int size){
    return terrain::surfaceTexelCoord(local, terrainSize, size);
}

// channel 0 of a height or density map texel, 0-1: 8-bit, 16-bit (R16) or 32-bit float (a linear
// .exr/.hdr map, raw or its CPU-readable cook)
static float readMapTexel(const unsigned char* pixels, size_t index, int bytesPerChannel){
    if (bytesPerChannel >= 4){
        float value;
        std::memcpy(&value, pixels + index, sizeof(value));
        return std::isfinite(value) ? value : 0.0f;
    }
    if (bytesPerChannel >= 2){
        // 16-bit little-endian (native unsigned short)
        const unsigned int value = static_cast<unsigned int>(pixels[index]) | (static_cast<unsigned int>(pixels[index + 1]) << 8);
        return static_cast<float>(value) / 65535.0f;
    }
    return pixels[index] / 255.0f;
}

float MeshSystem::sampleTerrainHeightTexels(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY){
    return terrain::sampleHeightTexels(pixels, width, height, channels, bytesPerChannel, texelX, texelY);
}

float MeshSystem::sampleTerrainHeight(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float terrainSize, float localX, float localZ){
    return terrain::sampleHeightLocal(pixels, width, height, channels, bytesPerChannel, terrainSize, localX, localZ);
}

bool MeshSystem::raycastTerrainSurface(const Ray& ray, TerrainComponent& terrain, Transform& transform, Vector3& worldPoint, bool ignoreHoles){
    // T3 R25: a crossing inside a hole is passed and the ray goes on; a brush sets ignoreHoles, to
    // paint and fill inside one
    return raycastTerrain(terrain, transform, ray, !ignoreHoles, worldPoint);
}

// Height range of an area of the heightmap, sampling every texel the vertex shader can
// read in it: a range shorter than the geometry makes node culling drop visible nodes.
void MeshSystem::getTerrainHeightRangeArea(TerrainComponent& terrain, float x, float z, float w, float h, float& minHeight, float& maxHeight){
    minHeight = 0.0f;
    maxHeight = 0.0f;

    if (w <= 0 || h <= 0 || terrain.terrainSize <= std::numeric_limits<float>::epsilon()){
        return;
    }

    // The heightmap may be empty, a framebuffer, or present-but-unloaded (a missing
    // or corrupt file leaves a null data pointer with needLoad already cleared).
    // Sample any of these as flat rather than dereferencing null data and crashing.
    if (terrain.heightMap.empty() || terrain.heightMap.isFramebuffer() || !terrain.heightMap.hasData()){
        return;
    }

    TextureData& textureData = terrain.heightMap.getData();
    const unsigned char* pixels = static_cast<const unsigned char*>(textureData.getData());
    const int width = textureData.getWidth();
    const int height = textureData.getHeight();
    const int channels = textureData.getChannels();
    const int bytesPerChannel = TextureData::getBytesPerChannel(textureData.getColorFormat());

    if (!pixels || width <= 0 || height <= 0 || channels <= 0){
        return;
    }

    auto sampleHeight = [&](int sampleX, int sampleZ){
        const size_t texelIndex = static_cast<size_t>(sampleZ) * static_cast<size_t>(width) + static_cast<size_t>(sampleX);
        const size_t index = texelIndex * static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
        return terrain.maxHeight * readMapTexel(pixels, index, bytesPerChannel);
    };

    // one texel of margin covers the bilinear filter of the vertices on the area border
    auto texelIndex = [&](float pos, int margin, int size){
        return std::clamp(static_cast<int>(std::floor(size * pos / terrain.terrainSize)) + margin, 0, size - 1);
    };

    const int firstX = texelIndex(x, -1, width);
    const int lastX = texelIndex(x + w, 1, width);
    const int firstZ = texelIndex(z, -1, height);
    const int lastZ = texelIndex(z + h, 1, height);

    minHeight = std::numeric_limits<float>::max();
    maxHeight = std::numeric_limits<float>::lowest();

    for (int j = firstZ; j <= lastZ; j++){
        for (int i = firstX; i <= lastX; i++){
            const float sampledHeight = sampleHeight(i, j);
            minHeight = std::min(minHeight, sampledHeight);
            maxHeight = std::max(maxHeight, sampledHeight);
        }
    }
}

void MeshSystem::createTerrainNode(TerrainComponent& terrain, float x, float y, float size, int lodDepth){
    TerrainNode& node = terrain.nodes[terrain.numNodes++];

    node.position = Vector2(x, y);
    node.size = size;

    node.currentRange = 0;
    node.resolution = terrain.resolution;

    float halfSize = size/2;

    if (lodDepth == 1){
        float terrainHalfSize = terrain.terrainSize / 2;
        float relativeX = x - halfSize + terrainHalfSize;
        float relativeY = y - halfSize + terrainHalfSize;

        node.hasChilds = false;

        getTerrainHeightRangeArea(terrain, relativeX, relativeY, size, size, node.minHeight, node.maxHeight);
    }else{
        float quarterSize = halfSize/2;

        node.childs[0] = terrain.numNodes;
        createTerrainNode(terrain, x - quarterSize, y - quarterSize, halfSize, lodDepth - 1);
        terrain.nodes[node.childs[0]].visible = false;

        node.childs[1] = terrain.numNodes;
        createTerrainNode(terrain, x - quarterSize, y + quarterSize, halfSize, lodDepth - 1);
        terrain.nodes[node.childs[1]].visible = false;

        node.childs[2] = terrain.numNodes;
        createTerrainNode(terrain, x + quarterSize, y - quarterSize, halfSize, lodDepth - 1);
        terrain.nodes[node.childs[2]].visible = false;

        node.childs[3] = terrain.numNodes;
        createTerrainNode(terrain, x + quarterSize, y + quarterSize, halfSize, lodDepth - 1);
        terrain.nodes[node.childs[3]].visible = false;

        node.hasChilds = true;

        node.maxHeight = std::max(std::max(terrain.nodes[node.childs[0]].maxHeight, terrain.nodes[node.childs[1]].maxHeight), std::max(terrain.nodes[node.childs[2]].maxHeight, terrain.nodes[node.childs[3]].maxHeight));
        node.minHeight = std::min(std::min(terrain.nodes[node.childs[0]].minHeight, terrain.nodes[node.childs[1]].minHeight), std::min(terrain.nodes[node.childs[2]].minHeight, terrain.nodes[node.childs[3]].minHeight));
    }
}

// Returns the terrain to the "unbuilt" state (the same one the empty-heightmap path
// produces). heightMapLoaded=false is what gates updateTerrain's LOD walk, so once a
// build is rejected or fails, no stale/undersized node tree or ranges array is ever
// indexed by terrainNodeLODSelect. Idempotent and safe whether or not a tree existed.
void MeshSystem::resetTerrainToUnbuilt(TerrainComponent& terrain, MeshComponent& mesh){
    terrain.heightMapLoaded = false;
    terrain.numNodes = 0;
    terrain.nodes.clear();
    for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
        for (int s = 0; s < 2; s++){
            terrain.views[v].nodesbuffer[s].clear();
        }
        terrain.views[v].needUpdateNodesBuffer = false;
    }
    mesh.verticesAABB = AABB::ZERO;
    mesh.aabb = AABB::ZERO;
    mesh.worldAABB = AABB::ZERO;
    mesh.needUpdateAABB = false;
    if (mesh.loaded || mesh.loadCalled){
        mesh.needReload = true;
    }
    terrain.needUpdateTerrain = false;
}

// a cook in a GPU format only (block-compressed): levels, no CPU pixels for the terrain's readers
static bool isGpuOnlyData(Texture& texture){
    if (texture.empty() || texture.isFramebuffer() || !texture.hasData()){
        return false;
    }
    TextureData& data = texture.getData();
    return data.hasLevels() && !data.getData();
}

bool MeshSystem::createTerrain(TerrainComponent& terrain, MeshComponent& mesh){
    applyTerrainMapSettings(terrain);
    // the heights are read on the CPU (collision, placement): a GPU-only cook is cooked again
    if (isGpuOnlyData(terrain.heightMap)){
        CookedTextures::requireCpuReadable(terrain.heightMap.getPath());
    }

    // Check heightmap loading state BEFORE clearing buffers to avoid leaving
    // the mesh with empty data when the heightmap is still loading.

    if (!terrain.heightMap.empty()){
        TextureLoadResult texResult = terrain.heightMap.load();
        if (texResult.state == ResourceLoadState::Loading){
            return false;
        }
    }
    // the collider and the queries read its texels as they read the heights (T3 R52)
    if (prepareTerrainHoleMap(terrain) == ResourceLoadState::Loading){
        return false;
    }

    for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
        for (int s = 0; s < 2; s++){
            terrain.views[v].nodesbuffer[s].clear();
            terrain.views[v].nodesbuffer[s].addAttribute(AttributeType::TERRAINNODEPOSITION, 2, true);
            terrain.views[v].nodesbuffer[s].addAttribute(AttributeType::TERRAINNODESIZE, 1, true);
            terrain.views[v].nodesbuffer[s].addAttribute(AttributeType::TERRAINNODERANGE, 1, true);
            terrain.views[v].nodesbuffer[s].addAttribute(AttributeType::TERRAINNODERESOLUTION, 1, true);
            terrain.views[v].nodesbuffer[s].setRenderAttributes(true);
            terrain.views[v].nodesbuffer[s].setInstanceBuffer(true);
            terrain.views[v].nodesbuffer[s].setUsage(BufferUsage::STREAM);
        }
    }

    prepareTerrainMesh(mesh);

    mesh.indices.clear();

    // T4 D1, D5: a World draws skirted grids from its pages, and builds no node tree
    if (terrain.world){
        terrain.nodes.clear();
        terrain.numNodes = 0;
        terrain.resolution = 32;
        mesh.numSubmeshes = 2;
        // the outer ring of a grid, flagged by y = 1 (includes/terrain_world_vs.glsl), each quad both ways
        auto addSkirt = [&](unsigned int submeshIndex, int segments){
            Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
            Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
            Attribute* attIndice = mesh.indices.getAttribute(AttributeType::INDEX);
            const Attribute& gridIndices = mesh.submeshes[submeshIndex].attributes[AttributeType::INDEX];
            const unsigned int firstGridVertex = (unsigned int)mesh.buffer.getCount() - (unsigned int)((segments + 1) * (segments + 1));
            std::vector<unsigned int> edge;
            for (int i = 0; i < segments; i++) edge.push_back(i);
            for (int i = 0; i < segments; i++) edge.push_back(segments + i * (segments + 1));
            for (int i = 0; i < segments; i++) edge.push_back((segments + 1) * (segments + 1) - 1 - i);
            for (int i = 0; i < segments; i++) edge.push_back((segments - i) * (segments + 1));
            const unsigned int firstRing = (unsigned int)mesh.buffer.getCount();
            const float step = 1.0f / (float)segments;
            for (unsigned int e : edge){
                const int ix = (int)(e % (segments + 1)), iy = (int)(e / (segments + 1));
                mesh.buffer.addVector3(attVertex, Vector3(ix * step - 0.5f, 1.0f, -(iy * step - 0.5f)));
                mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
            }
            unsigned int added = 0;
            const unsigned int count = (unsigned int)edge.size();
            for (unsigned int k = 0; k < count; k++){
                const unsigned int a = firstGridVertex + edge[k], b = firstGridVertex + edge[(k + 1) % count];
                const unsigned int ra = firstRing + k, rb = firstRing + (k + 1) % count;
                const unsigned int quad[12] = {a, ra, b, b, ra, rb, a, b, ra, b, rb, ra};
                for (unsigned int index : quad) mesh.indices.addUInt16(attIndice, (uint16_t)index);
                added += 12;
            }
            addSubmeshAttribute(mesh.submeshes[submeshIndex], "indices", AttributeType::INDEX, 1, AttributeDataType::UNSIGNED_SHORT,
                                gridIndices.getCount() + added, gridIndices.getOffset(), false);
        };
        createPlaneNodeSubmesh(0, mesh, 1, 1, (int)terrain.resolution, (int)terrain.resolution);
        addSkirt(0, (int)terrain.resolution);
        createPlaneNodeSubmesh(1, mesh, 1, 1, (int)terrain.resolution / 2, (int)terrain.resolution / 2);
        addSkirt(1, (int)terrain.resolution / 2);
        mesh.submeshes[1].generated = true;
        // the whole addressable extent: the selection culls inside it
        const float extent = 16384.0f;
        mesh.aabb = AABB(Vector3(-extent, std::min(0.0f, terrain.maxHeight) - 1.0f, -extent), Vector3(extent, 100000.0f, extent));
        terrain.heightMapLoaded = true;
        return true;
    }

    size_t idealSize = getTerrainGridArraySize(terrain.rootGridSize, terrain.levels);
    // createOrUpdateTerrain already caps idealSize; this catch is a last resort for a
    // genuine out-of-memory at an in-budget size, so the terrain fails to build (and
    // stops retrying) instead of aborting the process.
    try {
        terrain.nodes.resize(idealSize);
    } catch (const std::bad_alloc&) {
        // Fully unbuild: mesh.buffer/indices are already cleared above and grid[]/
        // heightMapLoaded still describe the previous tree, so leaving them would let
        // updateTerrain index an empty node vector. Reset (and stop retrying — a later
        // property change like lowering Levels re-arms needUpdateTerrain).
        Log::error("Terrain node allocation failed (%zu nodes). Lower Levels or Root Grid Size.", idealSize);
        resetTerrainToUnbuilt(terrain, mesh);
        return false;
    }

    mesh.numSubmeshes = 2;
    // fullRes submesh
    createPlaneNodeSubmesh(0, mesh, 1, 1, terrain.resolution, terrain.resolution);
    // halfRes submesh (internal LOD)
    createPlaneNodeSubmesh(1, mesh, 1, 1, terrain.resolution/2, terrain.resolution/2);
    mesh.submeshes[1].generated = true;

    updateTerrainAutoRanges(terrain);

    float rootNodeSize = terrain.terrainSize / terrain.rootGridSize;

    if (terrain.autoSetRanges){
        const CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera());
        if (camera && camera->farClip < (rootNodeSize * 2)){
            Log::warn("Terrain quadtree root is not in camera field of view. Increase terrain root grid.");
        }
    }

    terrain.numNodes = 0;

    // center terrain
    float offset = (terrain.terrainSize / 2) - (rootNodeSize / 2);

    terrain.numNodes = 0;
    for (int i = 0; i < terrain.rootGridSize; i++) {
        for (int j = 0; j < terrain.rootGridSize; j++) {
            float xPos = i*rootNodeSize;
            float zPos = j*rootNodeSize;

            terrain.grid[j + i*terrain.rootGridSize] = terrain.numNodes;
            createTerrainNode(terrain, xPos-offset, zPos-offset, rootNodeSize, terrain.levels);
        }
    }

    // Use abs/min-max so a degenerate (size 0) or inverted (negative size, negative maxHeight)
    // terrain never produces an inverted AABB (min > max), which would trip AABB's assertion.
    const float halfExtent = std::abs(terrain.terrainSize) * 0.5f;
    Vector3 min = Vector3(-halfExtent, std::min(0.0f, terrain.maxHeight), -halfExtent);
    Vector3 max = Vector3(halfExtent, std::max(0.0f, terrain.maxHeight), halfExtent);

    mesh.aabb = AABB(min, max);

    terrain.heightMapLoaded = true;

    return true;
}

// T5 R6: the foliage lives in FoliageSystem; these keep their callers
bool MeshSystem::setFoliagePreviewEntity(Entity entity){ return scene->getSystem<FoliageSystem>()->setPreviewEntity(entity); }
bool MeshSystem::hasPendingFoliageUpdates() const{ return scene->getSystem<FoliageSystem>()->hasPendingUpdates(); }
std::vector<Entity> MeshSystem::getFoliageEntities(Entity terrainEntity) const{ return scene->getSystem<FoliageSystem>()->getEntities(terrainEntity); }
Entity MeshSystem::getFoliageOwner(Entity foliageEntity) const{ return scene->getSystem<FoliageSystem>()->getOwner(foliageEntity); }
void MeshSystem::reloadFoliageMesh(const std::string& filename){ scene->getSystem<FoliageSystem>()->reloadMesh(filename); }
size_t MeshSystem::getFoliageInstanceCount(Entity terrainEntity, size_t layer) const{ return static_cast<size_t>(scene->getSystem<FoliageSystem>()->getInstanceCount(terrainEntity, static_cast<int>(layer))); }
std::string MeshSystem::getFoliageError(Entity terrainEntity, size_t layer) const{ return scene->getSystem<FoliageSystem>()->getLayerError(terrainEntity, static_cast<int>(layer)); }

void MeshSystem::retryFailedModelLoads(const std::string& filename){
    const std::string key = getModelFilenameKey(filename);
    for (auto it = failedModelLoads.begin(); it != failedModelLoads.end();){
        it = getModelFilenameKey(*it) == key ? failedModelLoads.erase(it) : std::next(it);
    }

    auto models = scene->getComponentArray<ModelComponent>();
    for (size_t i = 0; i < models->size(); i++){
        ModelComponent& model = models->getComponentFromIndex(i);
        const Entity entity = models->getEntity(i);
        // Foliage chunks load through their terrain layer instead
        if (model.gltfModel || model.objModel || model.filename.empty() || getModelFilenameKey(model.filename) != key ||
                getFoliageOwner(entity) != NULL_ENTITY || isAsyncModelLoadPending(entity, model.filename)){
            continue;
        }
        model.loadedFilename.clear();
        model.needUpdateModel = true;
    }
}

bool MeshSystem::loadPlacedModel(Entity entity, const std::string& path, bool asyncLoad){
    if (FileData::getFilePathExtension(path).compare("obj") == 0){
        return loadOBJ(entity, path, asyncLoad);
    }
    // All geometry on the instanced root, the file's node transforms baked in; the system
    // places the copy, not the file's root transform
    return loadGLTFModel(entity, path, asyncLoad, true, false, true, true);
}

// Sample height and normal in terrain-local space.
void MeshSystem::sampleTerrainSurface(TerrainComponent& terrain, float localX, float localZ, float& height, Vector3& normal){
    const terrain::SurfaceSample sample = querySurface(terrain, localX, localZ);
    height = sample.height;
    normal = sample.normal;
}

void MeshSystem::applyTerrainMapSettings(TerrainComponent& terrain){
    // The heightmap and the blend map span the whole 0..1 range, so their borders must read
    // the border texels, not the REPEAT blend with the opposite edge (detail maps are tiled).
    terrain.heightMap.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    terrain.heightMap.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    for (Texture& blendMap : terrain.blendMaps){
        blendMap.setWrapU(TextureWrap::CLAMP_TO_EDGE);
        blendMap.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    }
    // the terrain's heights are read back from its pixels
    terrain.heightMap.setReleaseDataAfterLoad(false);
}

void MeshSystem::updateTerrainAutoRanges(TerrainComponent& terrain){
    if (!terrain.autoSetRanges || !terrain.heightMapLoaded){
        return;
    }

    // a scene not loaded yet (an inline child scene) has no camera: the ranges wait for it
    const CameraComponent* cameraPtr = scene->findComponent<CameraComponent>(scene->getCamera());
    if (!cameraPtr){
        return;
    }
    const CameraComponent& camera = *cameraPtr;

    // Each level doubles the node size, so its range doubles too and every level keeps
    // the same detail on screen. Halving the far clip down instead leaves every range
    // wider than a terrain smaller than that far clip, drawing all of it at leaf level.
    float leafNodeSize = terrain.terrainSize / (terrain.rootGridSize * (1 << (terrain.levels - 1)));
    float firstLevel = leafNodeSize * 2;
    // the coarsest level still has to reach as far as the camera sees, or the terrain
    // disappears once it is farther away than the ranges its own size gives
    float lastLevel = std::max(camera.farClip, firstLevel * (1 << (terrain.levels - 1)));

    if (terrain.ranges.size() == static_cast<size_t>(terrain.levels) && terrain.ranges.front() == firstLevel && terrain.ranges.back() == lastLevel){
        return;
    }

    terrain.ranges.clear();
    terrain.ranges.resize(terrain.levels);
    terrain.ranges[0] = firstLevel;
    for (int i = 1; i < terrain.levels - 1; i++) {
        terrain.ranges[i] = terrain.ranges[i - 1] * 2;
    }
    terrain.ranges[terrain.levels - 1] = lastLevel;
}

void MeshSystem::createPlane(MeshComponent& mesh, float width, float depth, unsigned int tiles){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    float halfWidth = width / 2.0;
    float halfDepth = depth / 2.0;

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, 0, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, 0, halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, 0, halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, 0, -halfDepth));

    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));

    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));

    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));

    static const uint16_t indices_array[] = {
        0,  1,  2,
        0,  2,  3,
    };

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        6, (char*)&indices_array[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

// createPlane's quad (horizontal, +Y normal, uv along X and Z) as a grid of segments x
// segments quads with the same winding, uv and bounds
void MeshSystem::createGrid(MeshComponent& mesh, float width, float depth, unsigned int segments, unsigned int tiles){
    // 16-bit indices: at most 256 x 256 vertices
    segments = std::clamp(segments, 1u, 255u);

    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);

    const unsigned int row = segments + 1;
    for (unsigned int iz = 0; iz <= segments; iz++){
        const float tz = (float)iz / segments;
        for (unsigned int ix = 0; ix <= segments; ix++){
            const float tx = (float)ix / segments;
            mesh.buffer.addVector3(attVertex, Vector3((tx - 0.5f) * width, 0.0f, (tz - 0.5f) * depth));
            mesh.buffer.addVector2(attTexcoord, Vector2(tx * tiles, tz * tiles));
            mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
        }
    }

    // each quad as createPlane's: (x0, z0) (x0, z1) (x1, z1), then (x0, z0) (x1, z1) (x1, z0)
    std::vector<uint16_t> indices;
    indices.reserve(segments * segments * 6);
    for (unsigned int iz = 0; iz < segments; iz++){
        for (unsigned int ix = 0; ix < segments; ix++){
            const uint16_t a = (uint16_t)(iz * row + ix);
            const uint16_t b = (uint16_t)((iz + 1) * row + ix);
            const uint16_t c = (uint16_t)((iz + 1) * row + ix + 1);
            const uint16_t d = (uint16_t)(iz * row + ix + 1);
            indices.insert(indices.end(), {a, b, c, a, c, d});
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

// Vertical quad in the XY plane facing +Z (width along X, height along Y).
// Unlike createPlane (horizontal, +Y normal), a wall stands upright — its +Z
// normal matches the MirrorComponent default, so a mirror needs no rotation.
void MeshSystem::createWall(MeshComponent& mesh, float width, float height, unsigned int tiles){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    float halfWidth = width / 2.0;
    float halfHeight = height / 2.0;

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight, 0));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight, 0));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, halfHeight, 0));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, halfHeight, 0));

    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));

    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));

    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));

    static const uint16_t indices_array[] = {
        0,  1,  2,
        0,  2,  3,
    };

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        6, (char*)&indices_array[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createBox(MeshComponent& mesh, float width, float height, float depth, unsigned int tiles){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    float halfWidth = width / 2.0;
    float halfHeight = height / 2.0;
    float halfDepth = depth / 2.0;

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);

    // Front face (Z+)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, halfHeight,  halfDepth));

    // Back face (Z-)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight, -halfDepth));

    // Left face (X-)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight, -halfDepth));

    // Right face (X+)
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight, -halfDepth));

    // Top face (Y+)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight, -halfDepth));

    // Bottom face (Y-)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight, -halfDepth));

    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);

    // Front face (Z+)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));

    // Back face (Z-)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));

    // Left face (X-)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));

    // Right face (X+)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));

    // Top face (Y+)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));

    // Bottom face (Y-)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));

    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);

    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, -1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, -1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, -1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, -1.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(-1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(-1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(-1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(-1.0f, 0.0f, 0.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(1.0f, 0.0f, 0.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, -1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, -1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, -1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, -1.0f, 0.0f));

    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    for (int i = 0; i < 6; i++){
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    static const uint16_t indices_array[] = {
            // front
            0,  1,  2,
            0,  2,  3,
            // back
            4,  6,  5,
            4,  7,  6,
            // left
            8,  9, 10,
            8, 10, 11,
            // right
            12, 14, 13,
            12, 15, 14,
            // top
            16, 17, 18,
            16, 18, 19,
            // bottom
            20, 22, 21,
            20, 23, 22,
    };

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        36, (char*)&indices_array[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createSphere(MeshComponent& mesh, float radius, unsigned int slices, unsigned int stacks){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    float x, y, z, xz;                              // vertex position
    float nx, ny, nz, lengthInv = 1.0f / radius;    // vertex normal
    float s, t;                                     // vertex texCoord

    float sectorStep = 2 * M_PI / slices;
    float stackStep = M_PI / stacks;
    float sectorAngle, stackAngle;

    for(int i = 0; i <= stacks; ++i){
        stackAngle = M_PI / 2 - i * stackStep;      // starting from pi/2 to -pi/2
        xz = radius * cosf(stackAngle);             // r * cos(u)
        y = radius * sinf(stackAngle);              // r * sin(u)

        // add (sectorCount+1) vertices per stack
        // the first and last vertices have same position and normal, but different tex coords
        for(int j = 0; j <= slices; ++j){
            sectorAngle = j * sectorStep;           // starting from 0 to 2pi

            // vertex position (x, y, z)
            x = xz * sinf(sectorAngle);             // r * cos(u) * sin(v)
            z = -xz * cosf(sectorAngle);            // -r * cos(u) * cos(v)
            mesh.buffer.addVector3(attVertex, Vector3(x, y, z));

            // normalized vertex normal (nx, ny, nz)
            nx = x * lengthInv;
            ny = y * lengthInv;
            nz = z * lengthInv;
            mesh.buffer.addVector3(attNormal, Vector3(nx, ny, nz));

            // Z+ orientation means the texture faces the positive Z direction
            s = 0.5f + atan2f(x, z) / (2.0f * M_PI);

            // Remove Y flip since circle now starts at Z-
            t = (float)i / stacks;

            mesh.buffer.addVector2(attTexcoord, Vector2(s, t));

            // vertex color (white)
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // generate CCW index list of sphere triangles
    std::vector<uint16_t> indices;
    int k1, k2;
    for(int i = 0; i < stacks; ++i){
        k1 = i * (slices + 1);     // beginning of current stack
        k2 = k1 + slices + 1;      // beginning of next stack

        for(int j = 0; j < slices; ++j, ++k1, ++k2){
            // 2 triangles per sector excluding first and last stacks
            // k1 => k2 => k1+1
            if(i != 0){
                indices.push_back(k1);
                indices.push_back(k1 + 1);
                indices.push_back(k2);
            }

            // k1+1 => k2 => k2+1
            if(i != (stacks-1)){
                indices.push_back(k1 + 1);
                indices.push_back(k2 + 1);
                indices.push_back(k2);
            }
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createCylinder(MeshComponent& mesh, float baseRadius, float topRadius, float height, unsigned int slices, unsigned int stacks){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    float x, y, z;                                  // vertex position
    float radius;                                   // radius for each stack

    std::vector<float> sideNormals = getCylinderSideNormals(baseRadius, topRadius, height, slices);
    std::vector<float> unitCircleVertices = buildUnitCircleVertices(slices);

    // put vertices of side cylinder to array by scaling unit circle
    for(int i = 0; i <= stacks; ++i){
        y = -(height * 0.5f) + (float)i / stacks * height;      // vertex position y
        radius = baseRadius + (float)i / stacks * (topRadius - baseRadius);     // lerp
        float t = 1.0f - (float)i / stacks;   // top-to-bottom

        for(int j = 0, k = 0; j <= slices; ++j, k += 3){
            x = unitCircleVertices[k];
            z = unitCircleVertices[k+2];
            mesh.buffer.addVector3(attVertex, Vector3(x * radius, y, z * radius));
            mesh.buffer.addVector3(attNormal, Vector3(sideNormals[k], sideNormals[k+1], sideNormals[k+2]));
            mesh.buffer.addVector2(attTexcoord, Vector2(1.0f - (float)j / slices, t));
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // remember where the base.top vertices start
    unsigned int baseVertexIndex = mesh.buffer.getCount();

    // put vertices of base of cylinder
    y = -height * 0.5f;
    mesh.buffer.addVector3(attVertex, Vector3(0, y, 0));
    mesh.buffer.addVector3(attNormal, Vector3(0, -1, 0));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.5f, 0.5f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    for(int i = 0, j = 0; i < slices; ++i, j += 3){
        x = unitCircleVertices[j];
        z = unitCircleVertices[j+2];
        mesh.buffer.addVector3(attVertex, Vector3(x * baseRadius, y, z * baseRadius));
        mesh.buffer.addVector3(attNormal, Vector3(0, -1, 0));
        mesh.buffer.addVector2(attTexcoord, Vector2(x * 0.5f + 0.5f, -z * 0.5f + 0.5f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    // remember where the base vertices start
    unsigned int topVertexIndex = mesh.buffer.getCount();

    // put vertices of top of cylinder
    y = height * 0.5f;
    mesh.buffer.addVector3(attVertex, Vector3(0, y, 0));
    mesh.buffer.addVector3(attNormal, Vector3(0, 1, 0));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.5f, 0.5f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    for(int i = 0, j = 0; i < slices; ++i, j += 3){
        x = unitCircleVertices[j];
        z = unitCircleVertices[j+2];
        mesh.buffer.addVector3(attVertex, Vector3(x * topRadius, y, z * topRadius));
        mesh.buffer.addVector3(attNormal, Vector3(0, 1, 0));
        mesh.buffer.addVector2(attTexcoord, Vector2(-x * 0.5f + 0.5f, -z * 0.5f + 0.5f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    std::vector<uint16_t> indices;

    // put indices for sides
    unsigned int k1, k2;
    for(int i = 0; i < stacks; ++i){
        k1 = i * (slices + 1);     // bebinning of current stack
        k2 = k1 + slices + 1;      // beginning of next stack

        for(int j = 0; j < slices; ++j, ++k1, ++k2){
            // 2 trianles per sector
            indices.push_back(k1);
            indices.push_back(k2);
            indices.push_back(k1 + 1);

            indices.push_back(k2);
            indices.push_back(k2 + 1);
            indices.push_back(k1 + 1);
        }
    }

    // put indices for base
    for(int i = 0, k = baseVertexIndex + 1; i < slices; ++i, ++k){
        if(i < (slices - 1)){
            indices.push_back(baseVertexIndex);
            indices.push_back(k);
            indices.push_back(k + 1);
        }else{    // last triangle
            indices.push_back(baseVertexIndex);
            indices.push_back(k);
            indices.push_back(baseVertexIndex + 1);
        }
    }

    for(int i = 0, k = topVertexIndex + 1; i < slices; ++i, ++k)
    {
        if(i < (slices - 1)){
            indices.push_back(topVertexIndex);
            indices.push_back(k + 1);
            indices.push_back(k);
        }else{
            indices.push_back(topVertexIndex);
            indices.push_back(topVertexIndex + 1);
            indices.push_back(k);
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createCapsule(MeshComponent& mesh, float baseRadius, float topRadius, float height, unsigned int slices, unsigned int stacks){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    float stackAngle = 2 * M_PI / slices;

    // Top Hemisphere
    for (int i = 0; i <= stacks / 2; ++i) {
        float theta = i * M_PI / stacks;
        float y = topRadius * cos(theta);
        float r = topRadius * sin(theta);
        for (int j = 0; j <= slices; ++j) {
            float phi = j * stackAngle;
            float x = r * sin(phi);
            float z = -r * cos(phi);
            float s = (float)j / slices;
            float t = (float)(stacks / 2 - i) / (stacks / 2);

            mesh.buffer.addVector3(attVertex, Vector3(x, y + height / 2, z));
            mesh.buffer.addVector3(attNormal, Vector3(x / topRadius, y / topRadius, z / topRadius));
            mesh.buffer.addVector2(attTexcoord, Vector2(s, t));
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // Bottom Hemisphere
    for (int i = stacks / 2; i <= stacks; ++i) {
        float theta = i * M_PI / stacks;
        float y = baseRadius * cos(theta);
        float r = baseRadius * sin(theta);
        for (int j = 0; j <= slices; ++j) {
            float phi = j * stackAngle;
            float x = r * sin(phi);
            float z = -r * cos(phi);
            float s = (float)j / slices;
            float t = (float)(stacks - i) / (stacks / 2);

            mesh.buffer.addVector3(attVertex, Vector3(x, y - height / 2, z));
            mesh.buffer.addVector3(attNormal, Vector3(x / baseRadius, y / baseRadius, z / baseRadius));
            mesh.buffer.addVector2(attTexcoord, Vector2(s, t));
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    std::vector<uint16_t> indices;

    for (int i = 0; i < (stacks + 1); ++i) {
        for (int j = 0; j < (slices + 1); ++j) {
            int first = (i * (slices + 1)) + j;
            int second = first + slices + 1;

            indices.push_back(first);
            indices.push_back(first + 1);
            indices.push_back(second);

            indices.push_back(second);
            indices.push_back(first + 1);
            indices.push_back(second + 1);
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createTorus(MeshComponent& mesh, float radius, float ringRadius, unsigned int sides, unsigned int rings){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    const float two_pi = 2.0f * M_PI;
    const float dv = 1.0f / sides;
    const float du = 1.0f / rings;

    // generate vertices
    for (uint32_t side = 0; side <= sides; side++) {
        const float phi = (side * two_pi) / sides;
        const float sin_phi = sinf(phi);
        const float cos_phi = cosf(phi);
        for (uint32_t ring = 0; ring <= rings; ring++) {
            const float theta = (ring * two_pi) / rings;
            const float sin_theta = sinf(theta);
            const float cos_theta = cosf(theta);

            // torus surface position
            const float spx = sin_theta * (radius - (ringRadius * cos_phi));
            const float spy = sin_phi * ringRadius;
            const float spz = -cos_theta * (radius - (ringRadius * cos_phi));

            // torus position with ring-radius zero (for normal computation)
            const float ipx = sin_theta * radius;
            const float ipy = 0.0f;
            const float ipz = -cos_theta * radius;

            mesh.buffer.addVector3(attVertex, Vector3(spx, spy, spz));
            mesh.buffer.addVector3(attNormal, Vector3(spx - ipx, spy - ipy, spz - ipz));
            mesh.buffer.addVector2(attTexcoord, Vector2(ring * du, side * dv));
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // generate indices
    std::vector<uint16_t> indices;
    for (uint16_t side = 0; side < sides; side++) {
        const uint16_t row_a = side * (rings + 1);
        const uint16_t row_b = row_a + rings + 1;
        for (uint16_t ring = 0; ring < rings; ring++) {
            indices.push_back(row_a + ring);
            indices.push_back(row_a + ring + 1);
            indices.push_back(row_b + ring + 1);

            indices.push_back(row_a + ring);
            indices.push_back(row_b + ring + 1);
            indices.push_back(row_b + ring);
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

// A thumbnail: off the main thread, images capped (setImageDecodeMaxDimension), never a game's model
static bool isPreviewLoad(){
    return Engine::isAsyncThread() && g_imageDecodeMaxDimension > 0;
}

std::shared_ptr<tinygltf::Model> MeshSystem::parseGLTFModel(const std::string& filename){
    std::shared_ptr<tinygltf::Model> model = std::make_shared<tinygltf::Model>();

    std::string err;
    std::string warn;
    tinygltf::TinyGLTF loader;
    loader.SetFsCallbacks({&fileExists, &tinygltf::ExpandFilePath, &readWholeFile, &tinygltf::WriteWholeFile, &getFileSizeInBytes});
    loader.SetImagesAsIs(true); // keep images encoded; decode in parallel below

    const bool res = loadGLTFFile(loader, model.get(), &err, &warn, filename);

    if (!warn.empty())
        warn = withoutCookedImageWarnings(warn, *model, filename);
    if (!warn.empty()) {
        Log::warn("Loading GLTF model (%s): %s", filename.c_str(), warn.c_str());
    }

    if (!err.empty()) {
        Log::error("Can't load GLTF model (%s): %s", filename.c_str(), err.c_str());
        return nullptr;
    }

    if (!res) {
        Log::verbose("Failed to load glTF: %s", filename.c_str());
        return nullptr;
    }

    decodeGLTFImagesParallel(*model, g_imageDecodeMaxDimension);
    return model;
}

std::shared_ptr<ObjModelData> MeshSystem::parseOBJModel(const std::string& filename){
    std::shared_ptr<ObjModelData> model = std::make_shared<ObjModelData>();

    std::string warn;
    std::string err;
    tinyobj::FileReader::externalFunc = readFileToString;
    const std::string baseDir = FileData::getBaseDir(filename);
    const bool ret = tinyobj::LoadObj(&model->attrib, &model->shapes, &model->materials, &warn, &err, filename.c_str(), baseDir.c_str());

    if (!warn.empty()) {
        Log::warn("Loading OBJ model (%s): %s", filename.c_str(), warn.c_str());
    }

    if (!err.empty()) {
        Log::error("Can't load OBJ model (%s): %s", filename.c_str(), err.c_str());
        return nullptr;
    }

    return ret ? model : nullptr;
}

bool MeshSystem::isLoadOnMainThread() const{
    return Engine::isAsyncThread() && !isPreviewLoad() && !MainThreadQueue::isMainThread() && Engine::isSceneRunning(scene);
}

bool MeshSystem::prepareLoadOnMainThread(Entity entity, const std::string& filename, std::function<void()> commit){
    const std::string poolKey = getModelFilenameKey(filename);
    if (FileData::getFilePathExtension(filename) == "obj"){
        if (!ModelPool::getObj(poolKey)){
            std::shared_ptr<ObjModelData> parsed = parseOBJModel(filename);
            if (!parsed){
                return false;
            }
            ModelPool::addObj(poolKey, parsed);
        }
    }else if (!ModelPool::getGLTF(poolKey)){
        std::shared_ptr<tinygltf::Model> parsed = parseGLTFModel(filename);
        if (!parsed){
            return false;
        }
        ModelPool::addGLTF(poolKey, parsed);
    }

    Scene* target = scene;
    MainThreadQueue::post([lifetime = target->getLifetime(), target, entity, commit = std::move(commit)]() {
        if (!lifetime.expired() && target->isEntityCreated(entity)) {
            commit();
        }
    });
    return true;
}

bool MeshSystem::loadGLTF(Entity entity, const std::string filename, bool asyncLoad, bool skipEntities, bool changeRootTransform, bool bakeSceneRoot){
    return loadGLTFModel(entity, filename, asyncLoad, skipEntities, changeRootTransform, bakeSceneRoot, false);
}

bool MeshSystem::loadGLTFModel(Entity entity, const std::string& filename, bool asyncLoad, bool skipEntities, bool changeRootTransform,
                               bool bakeSceneRoot, bool placedCopy){
    if (isLoadOnMainThread()) {
        return prepareLoadOnMainThread(entity, filename, [this, entity, filename, asyncLoad, skipEntities, changeRootTransform, bakeSceneRoot, placedCopy]() {
            loadGLTFModel(entity, filename, asyncLoad, skipEntities, changeRootTransform, bakeSceneRoot, placedCopy);
        });
    }

    const std::string poolKey = getModelFilenameKey(filename);

    failedModelLoads.erase(filename); // an explicit load is a new attempt

    // If parsed data is already cached, skip the async background-thread entirely.
    if (asyncLoad && ModelPool::getGLTF(poolKey)){
        asyncLoad = false;
    }

    std::shared_ptr<AsyncModelLoadResult> asyncResult;
    if (asyncLoad){
        // a preload of the file fills ModelPool, parsing it here too would double the work
        if (isModelPreloading(filename)){
            return false;
        }
        asyncResult = pollOrStartAsyncModelLoad(scene, filename, false);
        if (!asyncResult || !asyncResult->success || !asyncResult->gltfModel){
            return false;
        }
    }

    ModelComponent& model = scene->getComponent<ModelComponent>(entity);

    // Taken before anything is torn down, put back once every mesh is filled.
    const SubmeshOverrides submeshOverrides = collectSubmeshOverrides(entity, model);

    // A merged root owns all renderable geometry. Remove any generated mesh-node entities left
    // by the previously loaded hierarchy before taking MeshComponent references: destroying a
    // child mesh can compact its component array and invalidate an already captured root reference.
    const bool preview = isPreviewLoad();
    if (model.mergeStaticMeshes && !preview && !model.meshNodesMapping.empty()) {
        clearMeshNodeMapping(model);
    }

    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    scene->getSystem<RenderSystem>()->prepareMeshForDataReload(entity, mesh);

    // another model file than the one loaded: its meshes take its default blend shape weights
    const bool otherModelFile = !model.loadedFilename.empty() && model.loadedFilename != poolKey;
    std::set<Entity> morphDefaultsApplied;

    destroyModel(model);

    model.filename = filename;

    uint64_t buildId = asyncLoad ? std::hash<std::string>{}(getAsyncModelLoadKey(filename)) : 0;

    std::vector<std::string> loadedBuffers;

    mesh.numExternalBuffers = 0;
    mesh.normAdjustJoint = 1.0f;
    mesh.normAdjustWeight = 1.0f;
    mesh.bonesMatrix.clear();
    mesh.needUpdateBones = false;
    mesh.morphTargetNames.clear();

    if (asyncLoad){
        if (!meshLoadHoldFile().empty()){
            std::lock_guard<std::mutex> lock(getMeshLoadHoldMutex());
            getMeshLoadHoldScenes().insert(scene);
        }
        // Async loader already produced a parsed model; reuse it (and cache it).
        auto cached = ModelPool::getGLTF(poolKey);
        if (cached){
            model.gltfModel = cached; // textures already pooled by the original load
        }else{
            model.gltfModel = asyncResult->gltfModel;
            ModelPool::addGLTF(poolKey, model.gltfModel);
            // Publish the texture pixels the worker already copied, so loadGLTFTexture below hits
            // the pool's reuse path instead of doing the big memcpy on this (main) thread.
            for (auto& prebuilt : asyncResult->prebuiltTextures){
                if (prebuilt.second){
                    TextureDataPool::put(prebuilt.first, prebuilt.second);
                }
            }
        }
    }else{
        // Try cache before hitting disk. A preview load (a thumbnail, its images decoded
        // downscaled) parses its own copy: it neither takes nor publishes the pooled model.
        const bool sharePool = (g_imageDecodeMaxDimension == 0);
        auto cached = sharePool ? ModelPool::getGLTF(poolKey) : nullptr;
        if (cached){
            model.gltfModel = cached;
        }else{
            model.gltfModel = parseGLTFModel(filename);
            if (!model.gltfModel) {
                return false;
            }

            const int maxImageDim = g_imageDecodeMaxDimension;

            // Only cache full-resolution parses. A downscaled preview decode must not be reused
            // as the full-resolution model in the user's scene.
            if (maxImageDim == 0 && sharePool) {
                ModelPool::addGLTF(poolKey, model.gltfModel);
            }
        }
    }

    // (a parse whose bytes were released, S53 R11: this load reads them)
    ModelCpuData::ensure(model.gltfModel, "a later load");

    // Every entity of the file shares the pooled model, and a load appends synthetic buffers to
    // it (once per key): loads of a pooled model take turns, a game's loading thread with the
    // main thread included
    std::unique_lock<std::recursive_mutex> pooledModelLock;
    if (ModelPool::getGLTF(poolKey) == model.gltfModel) {
        pooledModelLock = std::unique_lock<std::recursive_mutex>(getPooledGLTFMutex());
    }

    // the animations' sub-clip names, read here under the lock once: Model::findAnimation looks
    // names up from these, without the lock (loads never change the animations array)
    {
        std::vector<std::string> animationNames;
        animationNames.reserve(model.gltfModel->animations.size());
        for (const tinygltf::Animation& animation : model.gltfModel->animations) animationNames.push_back(animation.name);
        model.animationClipNames = anim::subClipNames(animationNames);
    }

    // A Vertex Animation mesh (.vat.glb, vertex animation spec 2.3): its _VAT_INDEX and, in
    // Bones mode, its JOINTS_0 / WEIGHTS_0 without a skin are what the Vertex Animator plays
    const bool vatGltf = std::find(model.gltfModel->extensionsUsed.begin(), model.gltfModel->extensionsUsed.end(),
                             vat::VAT_GLTF_EXTENSION) != model.gltfModel->extensionsUsed.end() ||
                         model.gltfModel->extensions.count(vat::VAT_GLTF_EXTENSION) != 0;
    mesh.vatMesh = vatGltf;

    // Build parent map and collect all nodes that own a mesh primitive.
    int meshNode    = -1; // first skinned mesh node — used for skeleton lookup
    std::vector<int> meshNodes;
    std::map<int, int> nodesParent;
    collectGLTFNodes(*model.gltfModel, nodesParent, meshNodes);
    for (int nodeIdx : meshNodes) {
        if (model.gltfModel->nodes[nodeIdx].skin >= 0) { meshNode = nodeIdx; break; }
    }

    // GPU buffers are a real limit: over it, what finds the pool full does not draw (SokolSystem)
    const size_t gpuBuffers = static_cast<size_t>(std::count_if(model.gltfModel->bufferViews.begin(), model.gltfModel->bufferViews.end(),
        [](const tinygltf::BufferView& view) { return view.target != 0 && view.name.rfind(GLTF_SYNTH_PREFIX, 0) != 0; }));
    if (!preview && gpuBuffers > static_cast<size_t>(Engine::getGpuResourceLimits().buffers)) {
        Log::error("Model %s has %zu vertex and index buffers, more than the maximum allowed (%i): what does not fit does not draw",
            filename.c_str(), gpuBuffers, Engine::getGpuResourceLimits().buffers);
    }

    if (meshNodes.empty()) {
        Log::error("GLTF model has no mesh nodes: %s", filename.c_str());
        if (asyncLoad) {
            ResourceProgress::failBuild(buildId);
        }
        return false;
    }

    // Models that need their glTF node hierarchy keep mesh nodes on child entities so animated
    // transform-only nodes and per-node placement survive save/load.
    bool anyNodeSkinned = false;
    for (int nodeIdx : meshNodes) {
        if (model.gltfModel->nodes[nodeIdx].skin >= 0) { anyNodeSkinned = true; break; }
    }
    const bool mergeMeshNodes = model.mergeStaticMeshes && meshNodes.size() > 1;
    if (mergeMeshNodes) {
        std::string reason;
        if (!canMergeStaticModel(model, &reason)) {
            Log::error("Cannot merge static GLTF model (%s): %s", filename.c_str(), reason.c_str());
            if (asyncLoad) {
                ResourceProgress::failBuild(buildId);
            }
            return false;
        }
    }
    // A thumbnail shows the model baked into one mesh (no nodes to build, nothing to pose or play).
    const bool canUseChildEntities = !mergeMeshNodes && !preview;
    // Under an Animation Player the sub-clip paths address one entity per glTF node (spec 9.R3).
    // A reload that keeps its entities keeps their layout: the editor reloads to switch it.
    const bool forceNodeHierarchy = canUseChildEntities && !skipEntities && isNodeHierarchyForced(entity);
    const bool useNodeHierarchy = canUseChildEntities &&
        (!model.nodesIdMapping.empty() || (!skipEntities &&
            (!model.gltfModel->animations.empty() || model.gltfModel->skins.size() > 1 || forceNodeHierarchy)));
    // an instanced node (EXT_mesh_gpu_instancing) draws its copies through its own entity's
    // InstancedMeshComponent, its node transform on that entity, never baked into the vertices
    bool anyNodeInstanced = false;
    for (int nodeIdx : meshNodes) {
        if (model.gltfModel->nodes[nodeIdx].extensions.count("EXT_mesh_gpu_instancing")) { anyNodeInstanced = true; break; }
    }
    const bool useChildEntities = canUseChildEntities &&
        (meshNodes.size() > 1 || useNodeHierarchy || anyNodeInstanced);

    // A preview bakes glTF node globals into a flat static mesh. Skinned nodes are baked too, but a
    // skinned mesh node's own transform is ignored (the skeleton drives it): they bake with the skin
    // root's default-pose matrix instead (joint global * inverse bind, identity for a consistent
    // export). Non-skinned nodes bake their own global transform.
    bool bakingFlatten = (preview && (meshNodes.size() > 1 || anyNodeSkinned))
        || mergeMeshNodes;
    // A copy a system places (loadPlacedModel) builds no skeleton and never animates its blend
    // shapes: it stands at rest as static geometry, its bones the identity and the blend shapes'
    // default weights baked into its positions and normals. Instanced, a textured mesh's blend
    // shapes alone need 17 vertex inputs, past the 16 every backend has.
    const bool restPose = placedCopy && !vatGltf;

    if (meshNode < 0) {
        meshNode = meshNodes[0];
    }

    // Prefer the GLTF scene root for the entity transform; fall back to the first mesh node.
    const int transformNode = gltfRootTransformNode(*model.gltfModel, meshNodes);

    Matrix4 matrix = getGLTFMeshGlobalMatrix(transformNode, model, nodesParent);

    // The transforms between the scene root and a lone mesh node, which the entity transform
    // (the scene root's, also when a scene load keeps the saved one) misses: baked into the
    // vertices like a merged model's nodes. An entity placed by something else (a foliage
    // chunk) bakes the node's whole global transform, as the merged nodes of a model do.
    Matrix4 meshOffset;
    if (!useChildEntities && !bakingFlatten && !anyNodeSkinned &&
            canBakeGLTFMesh(model, model.gltfModel->nodes[meshNodes[0]].mesh, nullptr, restPose)) {
        if (bakeSceneRoot) {
            meshOffset = getGLTFMeshGlobalMatrix(meshNodes[0], model, nodesParent);
        } else {
            int node = meshNodes[0];
            while (node >= 0 && node != transformNode) {
                meshOffset = getGLTFNodeMatrix(node, model) * meshOffset;
                node = nodesParent[node];
            }
            if (node < 0) {
                meshOffset = matrix.inverse() * meshOffset; // the mesh is under another scene root
            }
        }
    }
    const bool bakeMeshOffset = (meshOffset != Matrix4());

    // Child hierarchies carry glTF transforms themselves. The legacy multi-node skin path keeps
    // skinned children at identity, so it still needs the scene-root transform on the model.
    if (changeRootTransform && (!useChildEntities || (anyNodeSkinned && !useNodeHierarchy)) &&
            !bakingFlatten) {
        bool hasDefaultPosition = (transform.position == Vector3::ZERO);
        bool hasDefaultRotation = (transform.rotation == Quaternion::IDENTITY);
        bool hasDefaultScale    = (transform.scale    == Vector3::UNIT_SCALE);

        Vector3    newPosition;
        Vector3    newScale;
        Quaternion newRotation;
        matrix.decompose(newPosition, newScale, newRotation);

        if (hasDefaultPosition) transform.position = newPosition;
        if (hasDefaultRotation) transform.rotation = newRotation;
        if (hasDefaultScale)    transform.scale    = newScale;
        transform.needUpdate = true;
    }

    // cullingMode is the user's (a glTF has none to give): a load keeps it
    // from what places the vertices: the scene root (on the entity) and the baked offset; an
    // entity the root does not place has only the offset (none when the mesh cannot be baked)
    const Matrix4 placedMatrix = (bakeSceneRoot && !bakingFlatten) ? meshOffset : matrix * meshOffset;
    mesh.windingOrder = (placedMatrix.determinant() < 0.0) ? WindingOrder::CW : WindingOrder::CCW;
    // the part of that compensation that is the entity's (the scene root on the transform: the
    // baked offset and a flattened or baked root are in the vertices), which the draw then must
    // not compensate again (RenderSystem::isMirroredDraw)
    mesh.mirroredWinding = !bakeSceneRoot && !bakingFlatten && matrix.determinant() < 0.0;

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.4f); // Transform processing done
    }

    // Count total primitives to detect empty models early.
    unsigned int totalSubmeshes = 0;
    for (int nodeIdx : meshNodes) {
        int gltfMeshIndex = model.gltfModel->nodes[nodeIdx].mesh;
        if (isValidGLTFIndex(gltfMeshIndex, model.gltfModel->meshes)) {
            totalSubmeshes += static_cast<unsigned int>(model.gltfModel->meshes[gltfMeshIndex].primitives.size());
        }
    }

    if (totalSubmeshes == 0) {
        Log::error("GLTF model has no mesh primitives: %s", filename.c_str());
        if (asyncLoad) {
            ResourceProgress::failBuild(buildId);
        }
        return false;
    }

    model.morphNameMapping.clear();

    if (useChildEntities) {
        // Each node becomes its own child mesh; the root mesh stays an empty container (loadMesh
        // skips it). numExternalBuffers was already zeroed at the top of loadGLTF.
        mesh.numSubmeshes = 0;
    } else {
        // Flatten path: every node's primitives accumulate into the single root MeshComponent, so its
        // submesh array is sized for the total up-front and the buffer dedup/reserve happens once.
        mesh.numSubmeshes = totalSubmeshes;
        loadedBuffers.clear();
        if (model.gltfModel) {
            // One synthetic view per primitive for byte-index expansion, one for a VEC3 COLOR_0, up to
            // three more (position, normal, tangent) when baking node transforms, and up to eight sparse
            // morph attributes. Reserve so appends never invalidate non-owning external-buffer pointers.
            const size_t synthPerSubmesh = ((bakingFlatten || bakeMeshOffset) ? 5 : 2) + MAX_MORPHTARGETS;
            model.gltfModel->buffers.reserve(model.gltfModel->buffers.size() + mesh.numSubmeshes * synthPerSubmesh);
            model.gltfModel->bufferViews.reserve(model.gltfModel->bufferViews.size() + mesh.numSubmeshes * synthPerSubmesh);
        }
    }

    // A reload can hit a file whose nodes moved, so pair the children by name before the
    // index-keyed lookup below sends them to another node's geometry.
    if (skipEntities && useChildEntities && model.nodesIdMapping.empty()) {
        remapMeshNodesByName(model, meshNodes);
    }

    // Converted and baked copies are appended to the model, which ModelPool shares between every
    // entity of the file: one made by an earlier load is found by its key and reused, so loads
    // and instances do not each add their own.
    std::unordered_map<std::string, int> synthViews;
    for (size_t v = 0; v < model.gltfModel->bufferViews.size(); v++) {
        const std::string& name = model.gltfModel->bufferViews[v].name;
        if (name.rfind(GLTF_SYNTH_PREFIX, 0) == 0) {
            synthViews.emplace(name, static_cast<int>(v));
        }
    }
    auto synthView = [&](const std::string& key, const std::function<int()>& make) {
        auto found = synthViews.find(key);
        if (found != synthViews.end()) {
            return found->second;
        }
        const int view = make();
        if (isValidGLTFIndex(view, model.gltfModel->bufferViews)) {
            model.gltfModel->bufferViews[view].name = key;
            synthViews.emplace(key, view);
        }
        return view;
    };

    // Creating node meshes may reallocate the MeshComponent array. Do it only after the root mesh
    // setup above; the child path does not dereference that root reference again.
    if (useNodeHierarchy && !skipEntities &&
            !buildGLTFNodeHierarchy(entity, model, nodesParent)) {
        Log::error("Cannot create GLTF node hierarchy: %s", filename.c_str());
        if (asyncLoad)
            ResourceProgress::failBuild(buildId);
        return false;
    }

    std::set<int> morphLimitReported; // (the glTF meshes: once a load)

    unsigned int submeshIndex = 0; // continuous across nodes for the flatten path
    for (int nodeIdx : meshNodes) {
        int gltfMeshIndex = model.gltfModel->nodes[nodeIdx].mesh;
        if (!isValidGLTFIndex(gltfMeshIndex, model.gltfModel->meshes)) {
            continue;
        }

        tinygltf::Mesh& gltfmesh = model.gltfModel->meshes[gltfMeshIndex];

        // For the baked flatten path, precompute this node's world matrix (for positions) and its
        // normal matrix (inverse-transpose of the linear part) to bake into the vertices below.
        Matrix4 nodeBakeMatrix;
        Matrix3 nodeBakeNormalMatrix;
        if (bakingFlatten) {
            // Skinned nodes follow the skin root's default pose (a thumbnail approximation when
            // inverse binds are omitted); rigid nodes their own global transform.
            const int bakeSkin = model.gltfModel->nodes[nodeIdx].skin;
            if (isValidGLTFIndex(bakeSkin, model.gltfModel->skins) && !model.gltfModel->skins[bakeSkin].joints.empty()) {
                nodeBakeMatrix = getGLTFMeshGlobalMatrix(model.gltfModel->skins[bakeSkin].joints[0], model, nodesParent) *
                    getGLTFInverseBindMatrix(model, bakeSkin, 0);
            } else {
                nodeBakeMatrix = getGLTFMeshGlobalMatrix(nodeIdx, model, nodesParent);
                // a flattened preview draws an instanced node once, as its first instance
                std::vector<GltfInstance> instances;
                if (GltfNormalize::readInstances(*model.gltfModel, nodeIdx, instances) && !instances.empty()) {
                    nodeBakeMatrix = nodeBakeMatrix * gltfInstanceMatrix(instances[0]);
                }
            }
            nodeBakeNormalMatrix = nodeBakeMatrix.linear().inverse(1e-6f).transpose();
        } else if (bakeMeshOffset) {
            nodeBakeMatrix = meshOffset;
            nodeBakeNormalMatrix = nodeBakeMatrix.linear().inverse(1e-6f).transpose();
        }

        // Load into the root on the flat path, or the node's child mesh on the hierarchy path.
        Entity meshEntity = entity;
        if (useChildEntities) {
            meshEntity = NULL_ENTITY;
            auto existing = model.meshNodesMapping.find(nodeIdx);
            // A saved mapping outlives the entities it names: the ids of a scene rebuilt
            // from the factories are gone, and that is not an error to log
            if (existing != model.meshNodesMapping.end() &&
                    scene->findComponent<MeshComponent>(existing->second)) {
                meshEntity = existing->second; // reuse deserialized child (skipEntities reload)
            }
            if (meshEntity == NULL_ENTITY) {
                meshEntity = scene->createEntity();
                if (meshEntity == NULL_ENTITY) {
                    continue;
                }
                scene->addComponent<Transform>(meshEntity);
                scene->addComponent<MeshComponent>(meshEntity);

                const std::string& nodeName = model.gltfModel->nodes[nodeIdx].name;
                scene->setEntityName(meshEntity, nodeName.empty() ? ("MeshNode " + std::to_string(nodeIdx)) : nodeName);

                Transform& childTransform = scene->getComponent<Transform>(meshEntity);
                // Visibility only propagates to children on change, and these arrive well
                // after the root was set, so an invisible model would render every node
                if (Transform* rootTransform = scene->findComponent<Transform>(entity)) {
                    childTransform.visible = rootTransform->visible;
                }
                if (model.gltfModel->nodes[nodeIdx].skin >= 0) {
                    // Skinned nodes ignore their own transform (glTF spec): the skeleton drives the
                    // vertices in the model's space, so the child stays at identity under the root.
                    // Its bonesMatrix is filled from the shared skeleton at render time.
                    childTransform.position = Vector3::ZERO;
                    childTransform.rotation = Quaternion::IDENTITY;
                    childTransform.scale    = Vector3::UNIT_SCALE;
                } else {
                    // Flat under the root: the child carries its full gltf-space global matrix as its
                    // local transform, so root.world * childGlobal reproduces the node's placement.
                    Matrix4 nodeGlobal = getGLTFMeshGlobalMatrix(nodeIdx, model, nodesParent);
                    nodeGlobal.decompose(childTransform.position, childTransform.scale, childTransform.rotation);
                }

                scene->addEntityChild(entity, meshEntity, false);
                model.meshNodesMapping[nodeIdx] = meshEntity;
            }

            // Refresh the world matrix (also on reuse from a reopened scene) so the child doesn't
            // render at a stale transform until it is moved.
            scene->getComponent<Transform>(meshEntity).needUpdate = true;
        }

        MeshComponent& mesh = scene->getComponent<MeshComponent>(meshEntity); // shadows root mesh
        const int nodeSkinIndex = model.gltfModel->nodes[nodeIdx].skin;
        const tinygltf::Skin* nodeSkin = isValidGLTFIndex(nodeSkinIndex, model.gltfModel->skins)
            ? &model.gltfModel->skins[nodeSkinIndex] : nullptr;

        // Per-node fresh setup for the child path only. The flatten path keeps the root mesh's state
        // set once before the loop so all nodes accumulate (a continuous submeshIndex, shared buffers).
        if (useChildEntities) {
            loadedBuffers.clear(); // dedup is per child MeshComponent
            submeshIndex = 0;      // each child mesh starts at submesh 0

            mesh.numExternalBuffers = 0;
            mesh.normAdjustJoint = 1.0f;
            mesh.normAdjustWeight = 1.0f;
            mesh.bonesMatrix.clear();
            mesh.needUpdateBones = false;
            mesh.morphTargetNames.clear();
            mesh.vatMesh = vatGltf;

            mesh.numSubmeshes = static_cast<unsigned int>(gltfmesh.primitives.size());

            // Legacy skinned children stay at identity; hierarchy children use their node transform.
            double windingDet = nodeSkin && !useNodeHierarchy
                ? matrix.determinant()
                : getGLTFMeshGlobalMatrix(nodeIdx, model, nodesParent).determinant();
            mesh.windingOrder = (windingDet < 0.0) ? WindingOrder::CW : WindingOrder::CCW;
            // all of it on the entity transforms (the node's global, or the root's above a skinned child)
            mesh.mirroredWinding = windingDet < 0.0;

            // Reserve one synthetic byte-index view, one for a VEC3 COLOR_0, plus up to eight supported
            // sparse morph views per primitive so appends never invalidate non-owning buffer pointers.
            if (model.gltfModel) {
                const size_t synthPerSubmesh = 2 + MAX_MORPHTARGETS;
                model.gltfModel->buffers.reserve(model.gltfModel->buffers.size() + mesh.numSubmeshes * synthPerSubmesh);
                model.gltfModel->bufferViews.reserve(model.gltfModel->bufferViews.size() + mesh.numSubmeshes * synthPerSubmesh);
            }
        }

        if (nodeSkin && !nodeSkin->joints.empty() && mesh.bonesMatrix.validIndex(static_cast<int>(nodeSkin->joints.size()) - 1)){
            // one matrix per joint, the identity until a pose (the palette is the skin's; the
            // flatten path's one mesh takes its largest skin's)
            if (mesh.bonesMatrix.stored() < nodeSkin->joints.size())
                mesh.bonesMatrix.resize(nodeSkin->joints.size());
        }else if (!bakingFlatten && !restPose && nodeSkin && !nodeSkin->joints.empty()){
            Log::error("Model %s has a skin of %zu joints, more than %zu",
                filename.c_str(), nodeSkin->joints.size(), ARRAY_INDEX_LIMIT);
            if (asyncLoad) {
                ResourceProgress::failBuild(buildId);
            }
            return false;
        }

        for (size_t p = 0; p < gltfmesh.primitives.size(); p++) {
            // Claim this slot now so any early-continue still advances the index correctly.
            const unsigned int i = submeshIndex++;

            if (asyncLoad) {
                float submeshProgress = 0.4f + (0.4f * (float(i) / float(mesh.numSubmeshes)));
                ResourceProgress::updateProgress(buildId, submeshProgress);
            }

            mesh.submeshes[i].attributes.clear();
            mesh.submeshes[i].normAdjustJoint = 1.0f;
            mesh.submeshes[i].normAdjustWeight = 1.0f;
            mesh.submeshes[i].hasSkinningNormalization = false;
            mesh.submeshes[i].primitiveType = PrimitiveType::TRIANGLES;
            mesh.submeshes[i].material = Material();

            mesh.submeshes[i].overrideFields = 0; // the slot may carry an edit from a past load
            mesh.submeshes[i].vertexCount = 0;
            mesh.submeshes[i].bakedLods.reset();
            mesh.submeshes[i].bakedLodRecipe = 0;

            const tinygltf::Primitive& primitive = gltfmesh.primitives[p];

            if (!isValidGLTFIndex(primitive.indices, model.gltfModel->accessors)) {
                Log::error("GLTF primitive %u has no valid index accessor: %s", i, filename.c_str());
                continue;
            }

            const tinygltf::Accessor& indexAccessor = model.gltfModel->accessors[primitive.indices];
            if (!isValidGLTFIndex(indexAccessor.bufferView, model.gltfModel->bufferViews)) {
                Log::error("GLTF primitive %u has no valid index buffer view: %s", i, filename.c_str());
                continue;
            }
            // what RenderSystem::loadMesh will compute, here at once: a scene saved before it runs
            // (a new shader variant holds it for seconds on a GPU's compile threads) writes this
            // primitive's count, not the count of the slot's past load (the reverse below moves them)
            mesh.submeshes[i].vertexCount = static_cast<unsigned int>(indexAccessor.count);

            const tinygltf::Material* mat = nullptr;
            if (primitive.material >= 0) {
                if (isValidGLTFIndex(primitive.material, model.gltfModel->materials)) {
                    mat = &model.gltfModel->materials[primitive.material];
                } else {
                    Log::warn("GLTF primitive %u has invalid material index %i. Using default material: %s", i, primitive.material, filename.c_str());
                }
            }

            AttributeDataType indexType;
            int indexBufferView = indexAccessor.bufferView;
            size_t indexByteOffset = indexAccessor.byteOffset;
            if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                indexType = AttributeDataType::UNSIGNED_SHORT;
            } else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                indexType = AttributeDataType::UNSIGNED_INT;
            } else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                // No 8-bit index format in the renderer: expand to 16-bit via a synthetic buffer view.
                indexBufferView = synthView(gltfSynthKey("byteindex", indexAccessor), [&]() {
                    return convertGLTFByteIndicesToShort(indexAccessor, model);
                });
                if (!isValidGLTFIndex(indexBufferView, model.gltfModel->bufferViews)) {
                    Log::error("Failed to convert 8-bit indices for primitive %u: %s", i, filename.c_str());
                    continue;
                }
                indexType = AttributeDataType::UNSIGNED_SHORT;
                indexByteOffset = 0;
            } else {
                Log::error("Unknown index type %i", indexAccessor.componentType);
                continue;
            }

            // the extended lobes come from the file alone (an edit of them is put back by the
            // submesh overrides once every mesh is filled)
            mesh.submeshes[i].material.pbr = MaterialPbrExtensions();
            mesh.submeshes[i].material.emissiveStrength = 1.0f;

            if (mat) {
                // glTF texCoord selects the UV set per texture. Only two sets are
                // supported (a_texcoord1 / a_texcoord2); any higher index falls back
                // to the second set (clamped to 1).
                auto clampTexCoord = [](int texCoord) -> int { return (texCoord >= 1) ? 1 : 0; };

                if (!loadGLTFTexture(mat->pbrMetallicRoughness.baseColorTexture.index, model,
                        mesh.submeshes[i].material.baseColorTexture, filename + "|baseColorTexture"))
                    continue;
                mesh.submeshes[i].material.baseColorTexCoord = clampTexCoord(mat->pbrMetallicRoughness.baseColorTexture.texCoord);

                if (!loadGLTFTexture(mat->pbrMetallicRoughness.metallicRoughnessTexture.index, model,
                        mesh.submeshes[i].material.metallicRoughnessTexture, filename + "|metallicRoughnessTexture"))
                    continue;
                mesh.submeshes[i].material.metallicRoughnessTexCoord = clampTexCoord(mat->pbrMetallicRoughness.metallicRoughnessTexture.texCoord);

                if (!loadGLTFTexture(mat->occlusionTexture.index, model,
                        mesh.submeshes[i].material.occlusionTexture, filename + "|occlusionTexture"))
                    continue;
                mesh.submeshes[i].material.occlusionTexCoord = clampTexCoord(mat->occlusionTexture.texCoord);

                if (!loadGLTFTexture(mat->emissiveTexture.index, model,
                        mesh.submeshes[i].material.emissiveTexture, filename + "|emissiveTexture"))
                    continue;
                mesh.submeshes[i].material.emissiveTexCoord = clampTexCoord(mat->emissiveTexture.texCoord);

                if (!loadGLTFTexture(mat->normalTexture.index, model,
                        mesh.submeshes[i].material.normalTexture, filename + "|normalTexture"))
                    continue;
                mesh.submeshes[i].material.normalTexCoord = clampTexCoord(mat->normalTexture.texCoord);

                mesh.submeshes[i].material.baseColorFactor = Vector4(
                    mat->pbrMetallicRoughness.baseColorFactor[0],
                    mat->pbrMetallicRoughness.baseColorFactor[1],
                    mat->pbrMetallicRoughness.baseColorFactor[2],
                    mat->pbrMetallicRoughness.baseColorFactor[3]);

                mesh.submeshes[i].material.metallicFactor  = mat->pbrMetallicRoughness.metallicFactor;
                mesh.submeshes[i].material.roughnessFactor = mat->pbrMetallicRoughness.roughnessFactor;

                if (mat->alphaMode == "MASK") {
                    mesh.submeshes[i].material.alphaMode = MaterialAlphaMode::MASK;
                    mesh.submeshes[i].material.alphaCutoff = static_cast<float>(mat->alphaCutoff);
                } else if (mat->alphaMode == "BLEND") {
                    mesh.submeshes[i].material.alphaMode = MaterialAlphaMode::BLEND;
                } else {
                    mesh.submeshes[i].material.alphaMode = MaterialAlphaMode::ALPHA_OPAQUE;
                }

                mesh.submeshes[i].material.emissiveFactor = Vector3(
                    mat->emissiveFactor[0],
                    mat->emissiveFactor[1],
                    mat->emissiveFactor[2]);
                // leaf backlighting is authored as a glTF extra, there is no core material slot for it
                if (mat->extras.IsObject() && mat->extras.Has("jasperengine_foliage_transmission")) {
                    const auto& value = mat->extras.Get("jasperengine_foliage_transmission");
                    if (value.IsNumber() && std::isfinite(value.GetNumberAsDouble()))
                        mesh.submeshes[i].material.foliageTransmission = std::clamp((float)value.GetNumberAsDouble(), 0.0f, 1.0f);
                }
                // KHR_materials_* (MeshSystemGltfPbr.cpp; KHR_materials_unlit: the library Unlit
                // with the base colour, Lighting None)
                if (!loadGLTFMaterialExtensions(*mat, model, mesh.submeshes[i].material, primitive.attributes.count("COLOR_0") > 0))
                    continue;
            }

            // Alpha-masked glTF materials need the base-color alpha in the depth
            // pass as well, otherwise their invisible texels cast solid shadows.
            mesh.submeshes[i].textureShadow = mat && mat->alphaMode == "MASK";

            int indexStride = (indexType == AttributeDataType::UNSIGNED_SHORT) ? sizeof(uint16_t) : sizeof(uint32_t);

            mesh.submeshes[i].faceCulling = mat ? !mat->doubleSided : true;

            loadGLTFBuffer(indexBufferView, mesh, model, indexStride, loadedBuffers);
            addSubmeshAttribute(mesh.submeshes[i], getBufferName(indexBufferView, model),
                AttributeType::INDEX, 1, indexType, indexAccessor.count, indexByteOffset, false);
            // the detail levels the model cook baked (JE_mesh_lods, spec S54 contract C1)
            if (auto position = primitive.attributes.find("POSITION"); position != primitive.attributes.end() &&
                    isValidGLTFIndex(position->second, model.gltfModel->accessors)) {
                mesh.submeshes[i].bakedLods = MeshLodPool::readGltfLevels(*model.gltfModel, primitive, indexType,
                    model.gltfModel->accessors[position->second].count, mesh.submeshes[i].bakedLodRecipe);
            }

            for (auto& attrib : primitive.attributes) {
                if (!isValidGLTFIndex(attrib.second, model.gltfModel->accessors)) {
                    Log::warn("Invalid GLTF accessor index %i for attribute %s", attrib.second, attrib.first.c_str());
                    continue;
                }

                const tinygltf::Accessor& accessor = model.gltfModel->accessors[attrib.second];
                if (!isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
                    Log::warn("Invalid GLTF buffer view index %i for attribute %s", accessor.bufferView, attrib.first.c_str());
                    continue;
                }

                // Baked flatten (or a lone mesh node's offset from the scene root): redirect position,
                // normal, and tangent attributes to synthetic buffers with the node transform baked in.
                int attrBufferView = accessor.bufferView;
                size_t attrByteOffset = accessor.byteOffset;
                int byteStride = accessor.ByteStride(model.gltfModel->bufferViews[accessor.bufferView]);
                if ((bakingFlatten || restPose) && !vatGltf && (attrib.first == "JOINTS_0" || attrib.first == "WEIGHTS_0")) {
                    continue; // static geometry (a baked preview, a placed copy); drop the skinning attributes
                }
                // (the bakes write floats, also from a quantized normal or tangent)
                bool bakedToFloat = false;
                // a copy at rest: the displacements of the blend shapes its file weighs in
                std::vector<std::pair<int, float>> morphs;
                std::string morphKey;
                if (restPose && (attrib.first == "POSITION" || attrib.first == "NORMAL")) {
                    for (size_t t = 0; t < primitive.targets.size() && t < gltfmesh.weights.size(); t++) {
                        const float weight = static_cast<float>(gltfmesh.weights[t]);
                        auto target = primitive.targets[t].find(attrib.first);
                        if (weight == 0.0f || target == primitive.targets[t].end() ||
                                !isValidGLTFIndex(target->second, model.gltfModel->accessors)) {
                            continue;
                        }
                        tinygltf::Accessor& delta = model.gltfModel->accessors[target->second];
                        if (delta.sparse.isSparse && materializeGLTFSparseAccessor(delta, model) < 0) {
                            continue;
                        }
                        morphs.emplace_back(target->second, weight);
                        uint32_t bits;
                        std::memcpy(&bits, &weight, sizeof(bits));
                        morphKey += "|" + std::to_string(target->second) + ":" + std::to_string(bits);
                    }
                }
                if ((bakingFlatten || bakeMeshOffset || !morphs.empty()) && (attrib.first == "POSITION" || attrib.first == "NORMAL")) {
                    const bool isNormal = attrib.first == "NORMAL";
                    int baked = synthView(gltfSynthKey(isNormal ? "normal" : "position", accessor, &nodeBakeMatrix) + morphKey, [&]() {
                        return bakeGLTFTransformedAttribute(accessor, nodeBakeMatrix, nodeBakeNormalMatrix, isNormal, model, morphs);
                    });
                    if (baked >= 0) {
                        attrBufferView = baked;
                        attrByteOffset = 0;
                        byteStride = static_cast<int>(3 * sizeof(float));
                        bakedToFloat = true;
                    }
                }
                if ((bakingFlatten || bakeMeshOffset) && attrib.first == "TANGENT") {
                    int baked = synthView(gltfSynthKey("tangent", accessor, &nodeBakeMatrix), [&]() {
                        return bakeGLTFTransformedTangent(accessor, nodeBakeMatrix.linear(), model);
                    });
                    if (baked >= 0) {
                        attrBufferView = baked;
                        attrByteOffset = 0;
                        byteStride = static_cast<int>(4 * sizeof(float));
                        bakedToFloat = true;
                    }
                }
                // glTF also allows VEC3 colors; only the normalized integer ones need a copy
                bool colorConvertedToVec4 = false;
                if (attrib.first == "COLOR_0" && accessor.type == TINYGLTF_TYPE_VEC3) {
                    int converted = synthView(gltfSynthKey("color4", accessor), [&]() {
                        return convertGLTFColorToVec4(accessor, model);
                    });
                    if (converted >= 0) {
                        attrBufferView = converted;
                        attrByteOffset = 0;
                        byteStride = 4 * tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
                        colorConvertedToVec4 = true;
                    }
                }
                const std::string bufferName = getBufferName(attrBufferView, model);

                loadGLTFBuffer(attrBufferView, mesh, model, byteStride, loadedBuffers);

                int elements = (accessor.type != TINYGLTF_TYPE_SCALAR) ? accessor.type : 1;

                AttributeDataType dataType;
                if (bakedToFloat || accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    dataType = AttributeDataType::FLOAT;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE) {
                    dataType = AttributeDataType::BYTE;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    dataType = AttributeDataType::UNSIGNED_BYTE;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT) {
                    dataType = AttributeDataType::SHORT;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    dataType = AttributeDataType::UNSIGNED_SHORT;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                    // no vertex format binds a 32-bit integer (indices take their own path)
                    Log::warn("Not supported unsigned int of: %s", attrib.first.c_str());
                    continue;
                } else {
                    Log::error("Unknown data type %i of %s", accessor.componentType, attrib.first.c_str());
                    continue;
                }

                // a quantized normal padded to four components (KHR_mesh_quantization, kept by
                // GltfNormalize): bound as BYTE4N or SHORT4N, the shaders read xyz
                if (attrib.first == "NORMAL" && elements == 3 && accessor.normalized &&
                        (dataType == AttributeDataType::BYTE || dataType == AttributeDataType::SHORT) &&
                        byteStride >= 4 * tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType))) {
                    elements = 4;
                }

                bool attributeNormalized = !bakedToFloat && accessor.normalized;
                if (colorConvertedToVec4) {
                    elements = 4;
                }

                AttributeType attType;
                bool foundAttrs = false;
                if (attrib.first == "POSITION") {
                    mesh.vertexCount = accessor.count;
                    attType = AttributeType::POSITION;
                    foundAttrs = true;
                }
                if (attrib.first == "NORMAL") {
                    attType = AttributeType::NORMAL;
                    foundAttrs = true;
                }
                if (attrib.first == "TANGENT") {
                    attType = AttributeType::TANGENT;
                    foundAttrs = true;
                }
                if (attrib.first == "TEXCOORD_0") {
                    attType = AttributeType::TEXCOORD1;
                    foundAttrs = true;
                }
                if (attrib.first == "TEXCOORD_1") {
                    attType = AttributeType::TEXCOORD2;
                    foundAttrs = true;
                }
                if (attrib.first == "COLOR_0") {
                    if (elements == 4) {
                        attType = AttributeType::COLOR;
                        foundAttrs = true;
                        mesh.submeshes[i].hasVertexColor4 = true;
                    } else if (dataType == AttributeDataType::FLOAT) {
                        attType = AttributeType::COLOR;
                        foundAttrs = true;
                        mesh.submeshes[i].hasVertexColor3 = true;
                    } else {
                        Log::warn("Could not convert vector(3) of: %s", attrib.first.c_str());
                    }
                }

                if (attrib.first == "JOINTS_0") {
                    attType = AttributeType::BONEIDS;
                    foundAttrs = true;
                    mesh.submeshes[i].hasSkinningNormalization = true;
                    // Sokol has no non-normalized USHORT4 format — USHORT4N is always used,
                    // so the shader must expand the [0,1] value back to an integer index.
                    if (dataType == AttributeDataType::UNSIGNED_SHORT) {
                        mesh.normAdjustJoint = 65535.0;
                        mesh.submeshes[i].normAdjustJoint = 65535.0f;
                    } else if (dataType == AttributeDataType::UNSIGNED_BYTE) {
                        // `a_boneIds` is declared as vec4 in the shader, so use the normalized
                        // float path for byte joints as well and expand back in the shader.
                        mesh.normAdjustJoint = 255.0;
                        mesh.submeshes[i].normAdjustJoint = 255.0f;
                        attributeNormalized = true;
                    }
                }
                if (attrib.first == "WEIGHTS_0") {
                    attType = AttributeType::BONEWEIGHTS;
                    foundAttrs = true;
                    mesh.submeshes[i].hasSkinningNormalization = true;
                    if (accessor.normalized) {
                        if (dataType == AttributeDataType::BYTE)               mesh.submeshes[i].normAdjustWeight = 127.0f;
                        else if (dataType == AttributeDataType::UNSIGNED_BYTE) mesh.submeshes[i].normAdjustWeight = 255.0f;
                        else if (dataType == AttributeDataType::SHORT)         mesh.submeshes[i].normAdjustWeight = 32767.0f;
                        mesh.normAdjustWeight = mesh.submeshes[i].normAdjustWeight;
                    }
                    // Sokol always normalizes unsigned short
                    if (dataType == AttributeDataType::UNSIGNED_SHORT) {
                        mesh.normAdjustWeight = 65535.0;
                        mesh.submeshes[i].normAdjustWeight = 65535.0f;
                    }
                }

                // the vertex's row in a Vertex Animation payload (float, exact below 2^24)
                if (attrib.first == vat::VAT_INDEX_ATTRIBUTE) {
                    if (dataType == AttributeDataType::FLOAT && elements == 1) {
                        attType = AttributeType::VAT_INDEX;
                        foundAttrs = true;
                    } else {
                        Log::warn("Model attribute %s must be a float scalar: %s", attrib.first.c_str(), filename.c_str());
                        continue;
                    }
                }

                if (foundAttrs) {
                    for (int b = 0; b < mesh.numExternalBuffers; b++) {
                        if (mesh.eBuffers[b].getName() == bufferName)
                            mesh.eBuffers[b].setRenderAttributes(false);
                    }
                    addSubmeshAttribute(mesh.submeshes[i], bufferName, attType, elements, dataType,
                        accessor.count, attrByteOffset, attributeNormalized);
                } else if (attrib.first.rfind("TEXCOORD_", 0) != 0) {
                    // only two UV sets are supported (TEXCOORD_0/_1); silently ignore
                    // any higher TEXCOORD_n the model may carry
                    Log::warn("Model attribute unused: %s", attrib.first.c_str());
                }
            }

            bool morphTargets = false;
            bool morphNormals  = false;
            bool morphTangents = false;
            int  morphIndex    = 0;

            for (auto& morphs : primitive.targets) {
                if (restPose) {
                    break; // baked above
                }
                // Capacity must follow semantics in the prefix actually loaded. A later,
                // truncated target must not reduce the shader's attribute capacity.
                const int supportedMorphTargets = (morphNormals && morphTangents) ? 2
                                                : (morphNormals || morphTangents) ? 4
                                                : MAX_MORPHTARGETS;
                if (morphIndex >= supportedMorphTargets) {
                    if (!preview && morphLimitReported.insert(gltfMeshIndex).second) {
                        Log::error("Model %s: mesh '%s' has %zu blend shapes, more than the maximum allowed (%i): the ones over it are left out",
                            filename.c_str(), gltfmesh.name.c_str(), primitive.targets.size(), supportedMorphTargets);
                    }
                    break;
                }
                for (auto& attribMorph : morphs) {
                    morphTargets = true;

                    if (!isValidGLTFIndex(attribMorph.second, model.gltfModel->accessors)) {
                        Log::warn("Invalid GLTF accessor index %i for morph target %s", attribMorph.second, attribMorph.first.c_str());
                        continue;
                    }

                    tinygltf::Accessor& accessor = model.gltfModel->accessors[attribMorph.second];
                    if (accessor.sparse.isSparse && materializeGLTFSparseAccessor(accessor, model) < 0) {
                        continue;
                    }
                    if (!isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
                        Log::warn("Invalid GLTF buffer view index %i for morph target %s", accessor.bufferView, attribMorph.first.c_str());
                        continue;
                    }

                    int byteStride = accessor.ByteStride(model.gltfModel->bufferViews[accessor.bufferView]);
                    const std::string bufferName = getBufferName(accessor.bufferView, model);

                    loadGLTFBuffer(accessor.bufferView, mesh, model, byteStride, loadedBuffers);

                    int elements = (accessor.type != TINYGLTF_TYPE_SCALAR) ? accessor.type : 1;

                    AttributeDataType dataType;
                    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE) {
                        dataType = AttributeDataType::BYTE;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        dataType = AttributeDataType::UNSIGNED_BYTE;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT) {
                        dataType = AttributeDataType::SHORT;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        dataType = AttributeDataType::UNSIGNED_SHORT;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        dataType = AttributeDataType::UNSIGNED_INT;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                        dataType = AttributeDataType::FLOAT;
                    } else {
                        Log::error("Unknown data type %i of morph target %s", accessor.componentType, attribMorph.first.c_str());
                        continue;
                    }

                    AttributeType attType;
                    bool foundMorph = false;

                    if (attribMorph.first == "POSITION") {
                        if      (morphIndex == 0) { attType = AttributeType::MORPHTARGET0; foundMorph = true; }
                        else if (morphIndex == 1) { attType = AttributeType::MORPHTARGET1; foundMorph = true; }
                        else if ((!morphNormals && morphTangents) || (morphNormals && !morphTangents)) {
                            if      (morphIndex == 2) { attType = AttributeType::MORPHTARGET2; foundMorph = true; }
                            else if (morphIndex == 3) { attType = AttributeType::MORPHTARGET3; foundMorph = true; }
                        } else if (!morphNormals && !morphTangents) {
                            if      (morphIndex == 2) { attType = AttributeType::MORPHTARGET2; foundMorph = true; }
                            else if (morphIndex == 3) { attType = AttributeType::MORPHTARGET3; foundMorph = true; }
                            else if (morphIndex == 4) { attType = AttributeType::MORPHTARGET4; foundMorph = true; }
                            else if (morphIndex == 5) { attType = AttributeType::MORPHTARGET5; foundMorph = true; }
                            else if (morphIndex == 6) { attType = AttributeType::MORPHTARGET6; foundMorph = true; }
                            else if (morphIndex == 7) { attType = AttributeType::MORPHTARGET7; foundMorph = true; }
                        }
                    }
                    if (attribMorph.first == "NORMAL") {
                        morphNormals = true;
                        if      (morphIndex == 0) { attType = AttributeType::MORPHNORMAL0; foundMorph = true; }
                        else if (morphIndex == 1) { attType = AttributeType::MORPHNORMAL1; foundMorph = true; }
                        if (!morphTangents) {
                            if      (morphIndex == 2) { attType = AttributeType::MORPHNORMAL2; foundMorph = true; }
                            else if (morphIndex == 3) { attType = AttributeType::MORPHNORMAL3; foundMorph = true; }
                        }
                    }
                    if (attribMorph.first == "TANGENT") {
                        morphTangents = true;
                        if      (morphIndex == 0) { attType = AttributeType::MORPHTANGENT0; foundMorph = true; }
                        else if (morphIndex == 1) { attType = AttributeType::MORPHTANGENT1; foundMorph = true; }
                    }

                    if (foundMorph) {
                        for (int b = 0; b < mesh.numExternalBuffers; b++) {
                            if (mesh.eBuffers[b].getName() == bufferName)
                                mesh.eBuffers[b].setRenderAttributes(false);
                        }
                        addSubmeshAttribute(mesh.submeshes[i], bufferName, attType, elements, dataType,
                            accessor.count, accessor.byteOffset, accessor.normalized);
                    }
                }
                morphIndex++;
            }

            if (morphTargets) {
                // the file's default weights only on a freshly loaded model, or when another model
                // file is loaded: weights a scene or prefab saved (the editor's Blend Shapes rows, a
                // prefab instance override, all zeros too) or an earlier load of this model left
                // survive the (re)load (MeshComponent::morphWeightsSet). Every primitive of a mesh
                // passes here: the defaults apply to all of them in this load.
                // Kept weights cover the targets the mesh had at its last load
                // (MeshComponent::morphTargetCount): targets the file gained since get its defaults.
                const int targetCount = std::min(static_cast<int>(primitive.targets.size()), MAX_MORPHTARGETS);
                auto fileDefault = [&gltfmesh](int w) {
                    return w < static_cast<int>(gltfmesh.weights.size()) ? static_cast<float>(gltfmesh.weights[w]) : 0.0f;
                };
                if (!mesh.morphWeightsSet || otherModelFile || morphDefaultsApplied.count(meshEntity)) {
                    for (int w = 0; w < MAX_MORPHTARGETS; w++) {
                        mesh.morphWeights[w] = fileDefault(w);
                    }
                    mesh.morphWeightsSet = true;
                    // (the count of this load's primitives from here on)
                    if (morphDefaultsApplied.insert(meshEntity).second) mesh.morphTargetCount = 0;
                } else if (targetCount > mesh.morphTargetCount) {
                    for (int w = std::max(mesh.morphTargetCount, 0); w < targetCount; w++) {
                        mesh.morphWeights[w] = fileDefault(w);
                    }
                }
                mesh.morphTargetCount = std::max(mesh.morphTargetCount, targetCount);
                if (gltfmesh.extras.Has("targetNames") && gltfmesh.extras.Get("targetNames").IsArray()) {
                    // the model's table (Model:getMorphWeight by name) and this mesh's own
                    const tinygltf::Value& targetNames = gltfmesh.extras.Get("targetNames");
                    mesh.morphTargetNames.assign(static_cast<size_t>(targetNames.Size()), std::string());
                    for (int t = 0; t < targetNames.Size(); t++) {
                        const std::string name = targetNames.Get(t).IsString() ? targetNames.Get(t).Get<std::string>() : std::string();
                        model.morphNameMapping[name] = t;
                        mesh.morphTargetNames[static_cast<size_t>(t)] = name;
                    }
                }
            }
        }

        // Child meshes finalize here (per-node submesh count + reverse + AABB). The flatten path uses
        // a continuous submeshIndex across nodes and finalizes the root mesh once after the loop.
        if (useChildEntities) {
            mesh.numSubmeshes = submeshIndex; // reserved-but-skipped slots (invalid data) dropped
            mesh.submeshes.reverse(0, mesh.numSubmeshes);
            calculateMeshAABB(mesh);
            mesh.needReload = true;
            // EXT_mesh_gpu_instancing (S53 R3): the node's copies are the child mesh's instances
            std::vector<GltfInstance> instances;
            if (GltfNormalize::readInstances(*model.gltfModel, nodeIdx, instances)) {
                applyGLTFInstances(meshEntity, instances);
            }
        }

    }

    if (!useChildEntities) {
        // Flatten path: submeshIndex accumulated every node's primitives into the root mesh.
        mesh.numSubmeshes = submeshIndex;
    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.8f); // Submeshes processed
    }

    int skinIndex = -1;
    if (isValidGLTFIndex(meshNode, model.gltfModel->nodes)) {
        skinIndex = model.gltfModel->nodes[meshNode].skin;
    }
    int skeletonRoot = -1;

    if (skinIndex >= 0 && !useNodeHierarchy){
        if (!isValidGLTFIndex(skinIndex, model.gltfModel->skins)) {
            Log::warn("GLTF mesh node has invalid skin index %i: %s", skinIndex, filename.c_str());
            skinIndex = -1;
        }
    }

    if (skinIndex >= 0 && !useNodeHierarchy){
        tinygltf::Skin skin = model.gltfModel->skins[skinIndex];

        if (skin.skeleton >= 0 && isValidGLTFIndex(skin.skeleton, model.gltfModel->nodes)) {
            skeletonRoot = skin.skeleton;
        }else {
            //Find skeleton root
            for (int j = 0; j < skin.joints.size(); j++) {
                int nodeIndex = skin.joints[j];

                if (!isValidGLTFIndex(nodeIndex, model.gltfModel->nodes)) {
                    continue;
                }

                if (std::find(skin.joints.begin(), skin.joints.end(), nodesParent[nodeIndex]) == skin.joints.end())
                    skeletonRoot = nodeIndex;
            }
        }

        if (skeletonRoot >= 0) {
            // A thumbnail is baked, it has no skeleton to pose
            if (!skipEntities && !preview) {
                model.bonesNameMapping.clear();
                model.bonesIdMapping.clear();

                model.skeleton = generateSketetalStructure(entity, model, skeletonRoot, skinIndex);

                if (model.skeleton != NULL_ENTITY) {
                    scene->addEntityChild(entity, model.skeleton, false);
                }
            }

            // The bones and mesh nodes added Transforms since `transform` was taken: the array may
            // have grown (moved), so the root's is read again
            Transform& rootTransform = scene->getComponent<Transform>(entity);
            if (preview) {
                // Skinned previews are baked into static geometry above (skinning attributes
                // dropped), so there is no skeleton to pose and bonesMatrix stays at the identity it was
                // reset to at load start. Just refresh the transform so the preview gets framed.
                rootTransform.needUpdate = true;
            } else if (model.skeleton != NULL_ENTITY) {
                // Repopulate bonesMatrix before the first render frame. loadGLTF reset every bonesMatrix
                // to identity above, so RenderSystem must recompute the skinning matrices: flag the
                // model transform (so its inverseDerivedTransform is refreshed) and every bone transform
                // for update. This also covers reopening a saved project (skipEntities), where the
                // skeleton already exists from the deserialized scene; without it the skinning is left
                // stale and the mesh renders invisible until it is moved or the game is played and stopped.
                rootTransform.needUpdate = true;
                for (const auto& boneEntry : model.bonesIdMapping) {
                    Transform* boneTransform = scene->findComponent<Transform>(boneEntry.second);
                    if (boneTransform) {
                        boneTransform->needUpdate = true;
                    }
                }
            }
        }
    }

    if (!preview)
        buildGLTFSkinBindings(entity, model);

    // Animation tracks are also model-generated entities (a static preview never plays them).
    // Not either when an Animation Player plays the file's animations as sub-clips (spec 9.R15).
    if (!skipEntities && !preview) {
        model.animations.clear();

        const size_t animationCount = model.animationEntities ? model.gltfModel->animations.size() : 0;
        for (size_t i = 0; i < animationCount; i++) {
            const tinygltf::Animation &animation = model.gltfModel->animations[i];

            Entity anim;

            anim = scene->createEntity();
            if (anim == NULL_ENTITY) {
                continue;
            }
            scene->addComponent<ActionComponent>(anim);
            scene->addComponent<AnimationComponent>(anim);

            AnimationComponent& animcomp = scene->getComponent<AnimationComponent>(anim);

            std::string animName = animation.name.empty() ? "Animation " + std::to_string(i) : animation.name;
            scene->setEntityName(anim, animName);

            model.animations.push_back(anim);

            for (size_t j = 0; j < animation.channels.size(); j++) {

                const tinygltf::AnimationChannel &channel = animation.channels[j];
                if (!isValidGLTFIndex(channel.sampler, animation.samplers)) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid sampler", animation.name.c_str(), j);
                    continue;
                }

                const tinygltf::AnimationSampler &sampler = animation.samplers[channel.sampler];

                if (!isValidGLTFIndex(sampler.input, model.gltfModel->accessors) ||
                    !isValidGLTFIndex(sampler.output, model.gltfModel->accessors)) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid accessor", animation.name.c_str(), j);
                    continue;
                }

                tinygltf::Accessor accessorIn = model.gltfModel->accessors[sampler.input];
                if (!isValidGLTFIndex(accessorIn.bufferView, model.gltfModel->bufferViews)) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid input buffer view", animation.name.c_str(), j);
                    continue;
                }

                tinygltf::BufferView bufferViewIn = model.gltfModel->bufferViews[accessorIn.bufferView];

                tinygltf::Accessor accessorOut = model.gltfModel->accessors[sampler.output];
                if (!isValidGLTFIndex(accessorOut.bufferView, model.gltfModel->bufferViews)) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid output buffer view", animation.name.c_str(), j);
                    continue;
                }

                tinygltf::BufferView bufferViewOut = model.gltfModel->bufferViews[accessorOut.bufferView];

                if (!isValidGLTFIndex(bufferViewIn.buffer, model.gltfModel->buffers) ||
                    !isValidGLTFIndex(bufferViewOut.buffer, model.gltfModel->buffers) ||
                    accessorIn.count == 0 || accessorOut.count == 0) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid buffer data", animation.name.c_str(), j);
                    continue;
                }

                //TODO: Implement rotation and weights non float
                if (accessorOut.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {

                    // GLTF sampler interpolation (glTF 2.0): LINEAR (default), STEP or CUBICSPLINE
                    bool step = (sampler.interpolation.compare("STEP") == 0);
                    bool cubic = (sampler.interpolation.compare("CUBICSPLINE") == 0);
                    if (!step && !cubic && !sampler.interpolation.empty() && sampler.interpolation.compare("LINEAR") != 0) {
                        Log::warn("Animation: %s, channel %zu has unknown interpolation \"%s\", assuming LINEAR",
                                animation.name.c_str(), j, sampler.interpolation.c_str());
                    }

                    if (cubic && accessorIn.count < 2) {
                        // spec: a CUBICSPLINE sampler must have at least 2 keyframes
                        Log::warn("Cannot load animation: %s, channel %zu: CUBICSPLINE needs at least 2 keyframes",
                                animation.name.c_str(), j);
                        continue;
                    }

                    // CUBICSPLINE stores an (in-tangent, value, out-tangent) triple per
                    // keyframe, so its output accessor has 3x the input elements; weights
                    // additionally pack one scalar per morph target into each element.
                    size_t outPerKey = cubic ? 3 : 1;
                    if (channel.target_path.compare("weights") == 0) {
                        if (accessorOut.count % (accessorIn.count * outPerKey) != 0) {
                            Log::warn("Cannot load animation: %s, channel %zu has %zu output elements, not a multiple of %zu",
                                    animation.name.c_str(), j, accessorOut.count, accessorIn.count * outPerKey);
                            continue;
                        }
                    } else if (accessorOut.count != accessorIn.count * outPerKey) {
                        Log::warn("Cannot load animation: %s, channel %zu has %zu output elements, expected %zu",
                                animation.name.c_str(), j, accessorOut.count, accessorIn.count * outPerKey);
                        continue;
                    }

                    std::vector<unsigned char>& inputData = model.gltfModel->buffers[bufferViewIn.buffer].data;
                    std::vector<unsigned char>& outputData = model.gltfModel->buffers[bufferViewOut.buffer].data;
                    if (inputData.empty() || outputData.empty()) {
                        Log::warn("Cannot load animation: %s, channel %zu has empty buffer data", animation.name.c_str(), j);
                        continue;
                    }

                    float *timeValues = (float *) (inputData.data() + bufferViewIn.byteOffset + accessorIn.byteOffset);
                    float *values = (float *) (outputData.data() + bufferViewOut.byteOffset + accessorOut.byteOffset);

                    float duration = timeValues[accessorIn.count - 1];

                    Entity track;

                    track = scene->createEntity();
                    if (track == NULL_ENTITY) {
                        continue;
                    }

                    std::string trackName = animName + " " + channel.target_path;
                    if (channel.target_node >= 0 && channel.target_node < model.gltfModel->nodes.size()) {
                        std::string nodeName = model.gltfModel->nodes[channel.target_node].name;
                        if (!nodeName.empty()) {
                            trackName = nodeName + " " + channel.target_path;
                        }
                    }
                    scene->setEntityName(track, trackName);

                    scene->addComponent<ActionComponent>(track);
                    scene->addComponent<KeyframeTracksComponent>(track);

                    ActionComponent& actiontrack = scene->getComponent<ActionComponent>(track);
                    KeyframeTracksComponent& keyframe = scene->getComponent<KeyframeTracksComponent>(track);

                    if (step && accessorIn.count > 1) {
                        // GLTF STEP holds each key's value for its whole segment
                        keyframe.easings.assign(accessorIn.count - 1, EaseType::STEP);
                    }

                    // CUBICSPLINE element layout per keyframe: in-tangent, value, out-tangent
                    bool foundTrack = false;
                    if (channel.target_path.compare("translation") == 0) {
                        foundTrack = true;
                        scene->addComponent<TranslateTracksComponent>(track);
                        TranslateTracksComponent& translatetracks = scene->getComponent<TranslateTracksComponent>(track);
                        for (int c = 0; c < accessorIn.count; c++) {
                            const float* v = cubic ? &values[(9 * c) + 3] : &values[3 * c];
                            Vector3 positionAc(v[0], v[1], v[2]);

                            keyframe.times.push_back(timeValues[c]);
                            translatetracks.values.push_back(positionAc);
                            if (cubic) {
                                const float* a = &values[9 * c];
                                const float* b = &values[(9 * c) + 6];
                                translatetracks.inTangents.push_back(Vector3(a[0], a[1], a[2]));
                                translatetracks.outTangents.push_back(Vector3(b[0], b[1], b[2]));
                            }
                        }
                    }
                    if (channel.target_path.compare("rotation") == 0) {
                        foundTrack = true;
                        scene->addComponent<RotateTracksComponent>(track);
                        RotateTracksComponent& rotatetracks = scene->getComponent<RotateTracksComponent>(track);
                        for (int c = 0; c < accessorIn.count; c++) {
                            const float* v = cubic ? &values[(12 * c) + 4] : &values[4 * c];
                            Quaternion rotationAc(v[3], v[0], v[1], v[2]);

                            keyframe.times.push_back(timeValues[c]);
                            rotatetracks.values.push_back(rotationAc);
                            if (cubic) {
                                const float* a = &values[12 * c];
                                const float* b = &values[(12 * c) + 8];
                                rotatetracks.inTangents.push_back(Quaternion(a[3], a[0], a[1], a[2]));
                                rotatetracks.outTangents.push_back(Quaternion(b[3], b[0], b[1], b[2]));
                            }
                        }
                    }
                    if (channel.target_path.compare("scale") == 0) {
                        foundTrack = true;
                        scene->addComponent<ScaleTracksComponent>(track);
                        ScaleTracksComponent& scaletracks = scene->getComponent<ScaleTracksComponent>(track);
                        for (int c = 0; c < accessorIn.count; c++) {
                            const float* v = cubic ? &values[(9 * c) + 3] : &values[3 * c];
                            Vector3 scaleAc(v[0], v[1], v[2]);

                            keyframe.times.push_back(timeValues[c]);
                            scaletracks.values.push_back(scaleAc);
                            if (cubic) {
                                const float* a = &values[9 * c];
                                const float* b = &values[(9 * c) + 6];
                                scaletracks.inTangents.push_back(Vector3(a[0], a[1], a[2]));
                                scaletracks.outTangents.push_back(Vector3(b[0], b[1], b[2]));
                            }
                        }
                    }
                    if (channel.target_path.compare("weights") == 0) {
                        foundTrack = true;
                        scene->addComponent<MorphTracksComponent>(track);
                        MorphTracksComponent& morphtracks = scene->getComponent<MorphTracksComponent>(track);
                        int morphNum = accessorOut.count / (accessorIn.count * outPerKey);
                        for (int c = 0; c < accessorIn.count; c++) {
                            // CUBICSPLINE weights group per keyframe: a1..an, v1..vn, b1..bn
                            size_t base = (size_t)morphNum * outPerKey * c;
                            size_t vOff = base + (cubic ? morphNum : 0);
                            std::vector<float> weightsAc;
                            for (int m = 0; m < morphNum; m++) {
                                weightsAc.push_back(values[vOff + m]);
                            }

                            keyframe.times.push_back(timeValues[c]);
                            morphtracks.values.push_back(weightsAc);
                            if (cubic) {
                                std::vector<float> inTg;
                                std::vector<float> outTg;
                                for (int m = 0; m < morphNum; m++) {
                                    inTg.push_back(values[base + m]);
                                    outTg.push_back(values[base + (2 * morphNum) + m]);
                                }
                                morphtracks.inTangents.push_back(inTg);
                                morphtracks.outTangents.push_back(outTg);
                            }
                        }
                    }

                    if (foundTrack) {
                        auto targetNode = model.nodesIdMapping.find(channel.target_node);
                        if (targetNode != model.nodesIdMapping.end())
                            actiontrack.target = targetNode->second;
                        else if (model.bonesIdMapping.count(channel.target_node))
                            actiontrack.target = model.bonesIdMapping[channel.target_node];
                        else if (model.meshNodesMapping.count(channel.target_node))
                            actiontrack.target = model.meshNodesMapping[channel.target_node];
                        else
                            actiontrack.target = entity;
                        animcomp.actions.push_back({0, duration, track});
                    }else{
                        scene->destroyEntity(track);
                    }

                }else{
                    Log::error("Cannot load animation: %s, channel %zu: no float elements", animation.name.c_str(), j);
                }
            }

            // need to get here because other actions were created
            ActionComponent& anim_actioncomp = scene->getComponent<ActionComponent>(anim);

            anim_actioncomp.target = entity;

        }
    } // !skipEntities

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.95f); // Animations processed
    }

    // Root-mesh finalization (flat path only). In the child-entity path each child mesh was
    // already finalized in the loop, the root mesh is an empty container, and adding child
    // MeshComponents may have reallocated the component array — so the `mesh` reference captured
    // before the loop must not be dereferenced here.
    if (!useChildEntities) {
        mesh.submeshes.reverse(0, mesh.numSubmeshes);

        calculateMeshAABB(mesh);

        if (mesh.loaded)
            mesh.needReload = true;
    }

    // Only here are all submesh counts final, which is what the run ordinals are read from.
    applySubmeshOverrides(submeshOverrides, entity, model);

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 1.0f); // Complete
        ResourceProgress::completeBuild(buildId);
    }

    model.loadedFilename = getModelFilenameKey(filename);
    model.loadedMergeStaticMeshes = model.mergeStaticMeshes;

    // Read/Write (S53 R11): nothing at runtime reads this cooked model's mesh data
    CookedModelEntry cookedEntry;
    if (CookedModels::find(filename, cookedEntry) && !cookedEntry.keepCpuData && ModelPool::getGLTF(poolKey) == model.gltfModel)
        ModelCpuData::markReleasable(model.gltfModel, filename, poolKey);

    return true;
}

// An OBJ index is -1 when the face omits that attribute, and a corrupt file can point past the array
// the bytes of an OBJ parse loadOBJ builds (Buffer::setShareSource ids)
static const uint64_t OBJ_SHARE_VERTICES = 1;
static const uint64_t OBJ_SHARE_INDICES = 2;

static bool objIndexInRange(int index, size_t elements, size_t arraySize){
    return index >= 0 && (elements * static_cast<size_t>(index) + elements) <= arraySize;
}

static Vector3 objFaceNormal(const tinyobj::attrib_t& attrib, const std::vector<tinyobj::index_t>& indices, size_t offset, size_t numVertices){
    if (numVertices < 3){
        return Vector3::ZERO;
    }

    Vector3 corner[3];
    for (size_t i = 0; i < 3; i++){
        const int vertexIndex = indices[offset + i].vertex_index;
        corner[i] = Vector3(attrib.vertices[3 * vertexIndex + 0], attrib.vertices[3 * vertexIndex + 1], attrib.vertices[3 * vertexIndex + 2]);
    }

    return (corner[1] - corner[0]).crossProduct(corner[2] - corner[0]).normalized();
}

bool MeshSystem::loadOBJ(Entity entity, const std::string filename, bool asyncLoad){
    if (isLoadOnMainThread()) {
        return prepareLoadOnMainThread(entity, filename, [this, entity, filename, asyncLoad]() {
            loadOBJ(entity, filename, asyncLoad);
        });
    }

    const std::string poolKey = getModelFilenameKey(filename);

    failedModelLoads.erase(filename); // an explicit load is a new attempt

    // If parsed data is already cached, skip the async background-thread entirely.
    if (asyncLoad && ModelPool::getObj(poolKey)){
        asyncLoad = false;
    }

    std::shared_ptr<AsyncModelLoadResult> asyncResult;
    if (asyncLoad){
        // a preload of the file fills ModelPool, parsing it here too would double the work
        if (isModelPreloading(filename)){
            return false;
        }
        asyncResult = pollOrStartAsyncModelLoad(scene, filename, true);
        if (!asyncResult || !asyncResult->success || !asyncResult->objModel){
            return false;
        }
    }

    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    ModelComponent& model = scene->getComponent<ModelComponent>(entity);

    // Taken before the submeshes are rewritten, put back once they are filled.
    const SubmeshOverrides submeshOverrides = collectSubmeshOverrides(entity, model);

    scene->getSystem<RenderSystem>()->prepareMeshForDataReload(entity, mesh);

    destroyModel(model);

    model.filename = filename;

    uint64_t buildId = asyncLoad ? std::hash<std::string>{}(getAsyncModelLoadKey(filename)) : 0;

    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    std::string baseDir = FileData::getBaseDir(filename);

    if (asyncLoad){
        auto cached = ModelPool::getObj(poolKey);
        if (cached){
            model.objModel = cached;
        }else{
            model.objModel = asyncResult->objModel;
            ModelPool::addObj(poolKey, model.objModel);
        }
    }else{
        auto cached = ModelPool::getObj(poolKey);
        if (cached){
            model.objModel = cached;
        }else{
            model.objModel = parseOBJModel(filename);
            if (!model.objModel) {
                return false;
            }

            ModelPool::addObj(poolKey, model.objModel);
        }
    }

    tinyobj::attrib_t& attrib = model.objModel->attrib;
    std::vector<tinyobj::shape_t>& shapes = model.objModel->shapes;
    std::vector<tinyobj::material_t>& materials = model.objModel->materials;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    if (materials.size() > 0){
        mesh.numSubmeshes = materials.size();

    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.4f); // Materials setup
    }

    const bool hasMaterials = !materials.empty();

    for (size_t i = 0; i < mesh.numSubmeshes; i++) {
        // Update progress for material processing
        if (asyncLoad) {
            float materialProgress = 0.4f + (0.2f * (float(i) / float(mesh.numSubmeshes)));
            ResourceProgress::updateProgress(buildId, materialProgress);
        }

        mesh.submeshes[i].overrideFields = 0;
        applyDefaultObjMaterial(mesh.submeshes[i]);

        if (!hasMaterials) {
            continue;
        }

        // Convert the blinn-phong model to the pbr metallic-roughness model
        // Based on https://github.com/CesiumGS/obj2gltf
        const tinyobj::material_t& objMaterial = materials[i];
        const float specularIntensity = objMaterial.specular[0] * 0.2125 + objMaterial.specular[1] * 0.7154 + objMaterial.specular[2] * 0.0721; //luminance

        float roughnessFactor = objMaterial.shininess;
        roughnessFactor = roughnessFactor / 1000.0;
        roughnessFactor = 1.0 - roughnessFactor;
        roughnessFactor = std::min(std::max(roughnessFactor, 0.0f), 1.0f); //clamp

        if (specularIntensity < 0.1) {
            roughnessFactor *= (1.0 - specularIntensity);
        }

        const float metallicFactor = 0.0;

        // ------ End convertion

        const float dissolve = std::min(std::max(objMaterial.dissolve, 0.0f), 1.0f);
        // d 0 plus a diffuse map is a common OBJ export of "alpha is in the
        // texture", not an invisible mesh. Keep factor alpha at 1 in that case.
        const float factorAlpha = (dissolve <= 0.0f && !objMaterial.diffuse_texname.empty())
            ? 1.0f : dissolve;

        mesh.submeshes[i].material.baseColorFactor = Vector4(objMaterial.diffuse[0], objMaterial.diffuse[1], objMaterial.diffuse[2], factorAlpha);
        mesh.submeshes[i].material.emissiveFactor = Vector3(objMaterial.emission[0], objMaterial.emission[1], objMaterial.emission[2]);
        mesh.submeshes[i].material.metallicFactor = metallicFactor;
        mesh.submeshes[i].material.roughnessFactor = roughnessFactor;

        if (!objMaterial.diffuse_texname.empty())
            mesh.submeshes[i].material.baseColorTexture.setPath(baseDir+objMaterial.diffuse_texname);
        if (!objMaterial.normal_texname.empty())
            mesh.submeshes[i].material.normalTexture.setPath(baseDir+objMaterial.normal_texname);
        if (!objMaterial.emissive_texname.empty())
            mesh.submeshes[i].material.emissiveTexture.setPath(baseDir+objMaterial.emissive_texname);
        if (!objMaterial.ambient_texname.empty())
            mesh.submeshes[i].material.occlusionTexture.setPath(baseDir+objMaterial.ambient_texname);

        //TODO: occlusionFactor (Ka)
        //TODO: metallicroughnessTexture (map_Ks + map_Ns)

        // OBJ has no glTF alphaMode: dissolve < 1 is translucency; a diffuse
        // map is a cutout (window holes, foliage), not blended transparency.
        if (dissolve < 1){
            mesh.submeshes[i].material.alphaMode = MaterialAlphaMode::BLEND;
            mesh.transparent = true;
        } else if (!objMaterial.diffuse_texname.empty()){
            mesh.submeshes[i].material.alphaMode = MaterialAlphaMode::MASK;
        } else {
            mesh.submeshes[i].material.alphaMode = MaterialAlphaMode::ALPHA_OPAQUE;
        }
        mesh.submeshes[i].textureShadow =
            mesh.submeshes[i].material.alphaMode == MaterialAlphaMode::MASK;
    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.6f); // Materials processed, starting geometry
    }

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    size_t invalidFaces = 0;

    std::vector<std::vector<uint32_t>> indexMap;
    if (materials.size() > 0) {
        indexMap.resize(materials.size());
    }else{
        indexMap.resize(1);
    }
    std::vector<ObjCorner> corners;

    for (size_t i = 0; i < shapes.size(); i++) {
        // Update progress for shape processing
        if (asyncLoad) {
            float shapeProgress = 0.6f + (0.3f * (float(i) / float(shapes.size())));
            ResourceProgress::updateProgress(buildId, shapeProgress);
        }

        size_t index_offset = 0;
        for (size_t f = 0; f < shapes[i].mesh.num_face_vertices.size(); f++) {
            size_t fnum = shapes[i].mesh.num_face_vertices[f];

            int material_id = shapes[i].mesh.material_ids[f];
            if (material_id < 0 || material_id >= static_cast<int>(indexMap.size()))
                material_id = 0;

            bool validFace = true;
            bool missingNormals = false;
            for (size_t v = 0; v < fnum; v++) {
                const tinyobj::index_t& faceIdx = shapes[i].mesh.indices[index_offset + v];
                if (!objIndexInRange(faceIdx.vertex_index, 3, attrib.vertices.size())) validFace = false;
                if (!objIndexInRange(faceIdx.normal_index, 3, attrib.normals.size())) missingNormals = true;
            }

            // A face out of range would be a vertex at the origin, stretching the AABB and colliders
            if (!validFace) {
                invalidFaces++;
                index_offset += fnum;
                continue;
            }

            // Faces without their own normals are lit by the geometric one
            const Vector3 faceNormal = missingNormals ? objFaceNormal(attrib, shapes[i].mesh.indices, index_offset, fnum) : Vector3::ZERO;

            // For each vertex in the face
            for (size_t v = 0; v < fnum; v++) {
                tinyobj::index_t idx = shapes[i].mesh.indices[index_offset + v];

                indexMap[material_id].push_back(static_cast<uint32_t>(corners.size()));

                // Each attribute keeps its own count, so a missing value is written, never skipped
                const bool hasTexcoord = objIndexInRange(idx.texcoord_index, 2, attrib.texcoords.size());
                const bool hasNormal = objIndexInRange(idx.normal_index, 3, attrib.normals.size());
                const bool hasColor = objIndexInRange(idx.vertex_index, 3, attrib.colors.size());

                ObjCorner corner;
                corner.position = Vector3(attrib.vertices[3 * idx.vertex_index + 0],
                                          attrib.vertices[3 * idx.vertex_index + 1],
                                          attrib.vertices[3 * idx.vertex_index + 2]);
                corner.texcoord = hasTexcoord ? Vector2(attrib.texcoords[2 * idx.texcoord_index + 0],
                                                        1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]) : Vector2::ZERO;
                corner.normal = hasNormal ? Vector3(attrib.normals[3 * idx.normal_index + 0],
                                                    attrib.normals[3 * idx.normal_index + 1],
                                                    attrib.normals[3 * idx.normal_index + 2]) : faceNormal;
                corner.color = hasColor ? Vector4(attrib.colors[3 * idx.vertex_index + 0],
                                                  attrib.colors[3 * idx.vertex_index + 1],
                                                  attrib.colors[3 * idx.vertex_index + 2],
                                                  1.0) : Vector4(1.0, 1.0, 1.0, 1.0);
                corners.push_back(corner);
            }

            index_offset += fnum;
        }
    }

    if (invalidFaces > 0) {
        Log::warn("Model %s has %zu faces with out-of-range vertex indices, skipping them", filename.c_str(), invalidFaces);
    }

    // One vertex per face corner is what an OBJ face list gives: welded and ordered as a cooked
    // mesh is (docs/specs/S53-model-import-cook.md R7)
    {
        std::vector<bool> keepOrder(indexMap.size(), false);
        for (size_t i = 0; i < mesh.numSubmeshes && i < indexMap.size(); i++) {
            keepOrder[i] = mesh.submeshes[i].material.alphaMode == MaterialAlphaMode::BLEND;
        }
        optimizeObjCorners(corners, indexMap, keepOrder);
    }
    for (const ObjCorner& corner : corners) {
        mesh.buffer.addVector3(attVertex, corner.position);
        mesh.buffer.addVector2(attTexcoord, corner.texcoord);
        mesh.buffer.addVector3(attNormal, corner.normal);
        mesh.buffer.addVector4(attColor, corner.color);
    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.9f); // Geometry processed
    }

    const bool useUint32Indices = mesh.buffer.getCount() > (static_cast<unsigned int>(std::numeric_limits<uint16_t>::max()) + 1u);

    mesh.indices.clear();
    mesh.indices.setStride(useUint32Indices ? sizeof(uint32_t) : sizeof(uint16_t));

    const AttributeDataType indexDataType = useUint32Indices ? AttributeDataType::UNSIGNED_INT : AttributeDataType::UNSIGNED_SHORT;
    const size_t indexTypeSize = useUint32Indices ? sizeof(uint32_t) : sizeof(uint16_t);

    for (size_t i = 0; i < mesh.numSubmeshes; i++) {
        addSubmeshAttribute(mesh.submeshes[i], "indices", AttributeType::INDEX, 1, indexDataType, indexMap[i].size(), mesh.indices.getCount() * indexTypeSize, false);

        if (indexMap[i].size() > 0){
            if (useUint32Indices) {
                mesh.indices.setValues(mesh.indices.getCount(), mesh.indices.getAttribute(AttributeType::INDEX), indexMap[i].size(), (char*)&indexMap[i].front(), sizeof(uint32_t));
            } else {
                std::vector<uint16_t> u16indices(indexMap[i].size());
                for (size_t j = 0; j < indexMap[i].size(); j++)
                    u16indices[j] = static_cast<uint16_t>(indexMap[i][j]);
                mesh.indices.setValues(mesh.indices.getCount(), mesh.indices.getAttribute(AttributeType::INDEX), u16indices.size(), (char*)&u16indices.front(), sizeof(uint16_t));
            }
            mesh.indices.setRenderAttributes(false);
        }
    }

    // built from the parse alone: every Model of the file draws the same bytes, from one GPU
    // vertex and one index buffer (MeshBufferPool)
    const std::shared_ptr<const void> objSource = std::static_pointer_cast<const void>(model.objModel);
    mesh.buffer.setShareSource(objSource, OBJ_SHARE_VERTICES);
    mesh.indices.setShareSource(objSource, OBJ_SHARE_INDICES);

    mesh.submeshes.reverse(0, mesh.numSubmeshes);

    applySubmeshOverrides(submeshOverrides, entity, model);

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 1.0f); // Complete
        ResourceProgress::completeBuild(buildId);
    }

    model.loadedFilename = getModelFilenameKey(filename);
    model.loadedMergeStaticMeshes = model.mergeStaticMeshes;

    return true;
}

void MeshSystem::applyGLTFInstances(Entity entity, const std::vector<GltfInstance>& instances){
    createInstancedMesh(entity);
    InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);
    instmesh.instances.clear();
    instmesh.instances.reserve(instances.size());
    for (const GltfInstance& instance : instances) {
        instmesh.instances.push_back(gltfInstanceData(instance));
    }
    // the GPU instance buffer holds them all (the mesh loads after this)
    instmesh.maxInstances = std::max(1u, static_cast<unsigned int>(instances.size()));
    instmesh.needUpdateInstances = true;
}

void MeshSystem::createInstancedMesh(Entity entity){
    Signature signature = scene->getSignature(entity);

    if (!signature.test(scene->getComponentId<InstancedMeshComponent>())){
        scene->addComponent<InstancedMeshComponent>(entity);
    }
}

void MeshSystem::removeInstancedMesh(Entity entity){
    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<InstancedMeshComponent>())){
        scene->removeComponent<InstancedMeshComponent>(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            mesh.aabb = mesh.verticesAABB;
            if (mesh.loaded)
                mesh.needReload = true;
        }
    }
}

bool MeshSystem::hasInstancedMesh(Entity entity) const{
    Signature signature = scene->getSignature(entity);

    return signature.test(scene->getComponentId<InstancedMeshComponent>());
}

bool MeshSystem::isNodeHierarchyForced(Entity entity) const{
    // the entity and its Transform ancestors (bounded: a broken parent loop ends the walk)
    Entity current = entity;
    for (int depth = 0; current != NULL_ENTITY && depth < 65536; depth++){
        if (!scene->isEntityCreated(current)){
            break;
        }
        if (scene->findComponent<AnimationPlayerComponent>(current)){
            return true;
        }
        const Transform* transform = scene->findComponent<Transform>(current);
        if (!transform){
            break;
        }
        current = transform->parent;
    }
    return false;
}

bool MeshSystem::isLegacyLayout(const ModelComponent& model){
    // not loaded yet: its first load builds the forced layout itself
    if (model.filename.empty() || model.loadedFilename.empty() || !model.nodesIdMapping.empty()){
        return false;
    }
    // OBJ files have no nodes
    std::string ext = FileData::getFilePathExtension(model.filename);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (ext == "obj"){
        return false;
    }
    // a merged static model (more than one mesh node, as loadGLTF merges) animates only at the root
    if (model.mergeStaticMeshes){
        size_t meshNodes = 0;
        if (model.gltfModel){
            for (const tinygltf::Node& node : model.gltfModel->nodes){
                if (node.mesh >= 0) meshNodes++;
            }
        }
        if (!model.gltfModel || meshNodes > 1){
            return false;
        }
    }
    return true;
}

bool MeshSystem::needsNodeHierarchyReload(Entity entity) const{
    const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
    return model && isLegacyLayout(*model) && isNodeHierarchyForced(entity);
}

bool MeshSystem::getLegacyRootTransform(Entity entity, Matrix4& sceneRoot){
    ModelComponent* model = scene->findComponent<ModelComponent>(entity);
    if (!model || !model->gltfModel || !model->nodesIdMapping.empty() || model->gltfModel->nodes.empty()){
        return false;
    }
    const tinygltf::Model& gltf = *model->gltfModel;
    std::map<int, int> nodesParent;
    std::vector<int> meshNodes;
    collectGLTFNodes(gltf, nodesParent, meshNodes);
    bool anyNodeSkinned = false;
    for (int nodeIdx : meshNodes){
        anyNodeSkinned = anyNodeSkinned || gltf.nodes[nodeIdx].skin >= 0;
    }
    // loadGLTF's condition for the root transform on the entity in the legacy layout (`!useChildEntities
    // || anyNodeSkinned`: child meshes of a static multi-mesh model carry their whole global matrix)
    if (meshNodes.empty() || (meshNodes.size() > 1 && !anyNodeSkinned)){
        return false;
    }
    sceneRoot = getGLTFMeshGlobalMatrix(gltfRootTransformNode(gltf, meshNodes), *model, nodesParent);
    return true;
}

void MeshSystem::clearBoneMapping(ModelComponent& model){
    if (model.nodesIdMapping.empty()) {
        for (auto const& bone : model.bonesIdMapping){
            if (scene->isEntityCreated(bone.second))
                scene->destroyEntity(bone.second);
        }
    }
    model.bonesIdMapping.clear();
    model.bonesNameMapping.clear();

    model.skeleton = NULL_ENTITY;
}

void MeshSystem::clearAnimationMapping(ModelComponent& model){
    for (int i = 0; i < model.animations.size(); i++){
        if (scene->isEntityCreated(model.animations[i]))
            scene->destroyEntity(model.animations[i]);
    }
    model.animations.clear();
}

void MeshSystem::clearMeshNodeMapping(ModelComponent& model){
    if (model.nodesIdMapping.empty()) {
        for (auto const& node : model.meshNodesMapping){
            if (scene->isEntityCreated(node.second))
                scene->destroyEntity(node.second);
        }
    } else {
        for (const auto& node : model.nodesIdMapping) {
            Entity nodeEntity = node.second;
            if (scene->isEntityCreated(nodeEntity))
                scene->destroyEntity(nodeEntity);
        }
        model.nodesIdMapping.clear();
        model.bonesIdMapping.clear();
        model.bonesNameMapping.clear();
        model.skeleton = NULL_ENTITY;
    }
    model.meshNodesMapping.clear();
    model.skinBindings.clear();
}

void MeshSystem::destroyModel(ModelComponent& model){
    if (model.gltfModel){
        model.gltfModel.reset();
    }
    model.animationClipNames.clear();

    if (model.objModel){
        model.objModel.reset();
    }

    if (!model.loadedFilename.empty()){
        ModelPool::removeGLTF(model.loadedFilename);
        ModelPool::removeObj(model.loadedFilename);
    }

    model.morphNameMapping.clear();
    model.skinBindings.clear();

    model.filename = "";
    model.loadedFilename.clear();
    model.loadedMergeStaticMeshes = false;
}

void MeshSystem::resetModelToBindPose(Entity entity, ModelComponent& model){
    if (model.gltfModel && !model.nodesIdMapping.empty()) {
        std::map<int, Entity> defaults = getModelNodeDefaultParents(entity, model);
        for (const auto& node : model.nodesIdMapping) {
            Transform* transform = scene->findComponent<Transform>(node.second);
            if (!transform || !isValidGLTFIndex(node.first, model.gltfModel->nodes))
                continue;
            // A part the user moved keeps its own local; the file's local is relative to another parent
            auto defaultParent = defaults.find(node.first);
            if (defaultParent != defaults.end() && transform->parent != defaultParent->second)
                continue;
            getGLTFNodeMatrix(node.first, model).decompose(
                transform->position, transform->scale, transform->rotation);
            transform->needUpdate = true;
        }
        return;
    }

    for (auto const& bone : model.bonesIdMapping){
        Entity boneEntity = bone.second;
        BoneComponent* boneComp = scene->findComponent<BoneComponent>(boneEntity);
        Transform* transform = scene->findComponent<Transform>(boneEntity);
        if (!boneComp || !transform){
            continue;
        }

        transform->position = boneComp->bindPosition;
        transform->rotation = boneComp->bindRotation;
        transform->scale = boneComp->bindScale;
        transform->needUpdate = true;
    }
}

// Sprite:playTag (S44 R38): the tag's frames for the sheet's durations, at the tag's speed;
// a zero-duration frame shows for one update
void MeshSystem::stepSpriteTag(SpriteComponent& sprite, double dt){
    if (!sprite.sheetData || sprite.tagFrames.empty()){
        return;
    }
    const SpriteSheetData& sheet = *sprite.sheetData;
    const double step = dt * 1000.0 * sprite.tagSpeed;
    if (step <= 0.0){
        return;
    }
    if (!std::isfinite(step)){
        return;
    }
    if (sprite.tagStep >= sprite.tagFrames.size()){
        sprite.tagStep = 0;
    }
    auto durationOf = [&](size_t i) -> double {
        const int f = sprite.tagFrames[i];
        return (f >= 0 && f < sheet.frameCount()) ? (double)sheet.frames[(size_t)f].durationMs : 0.0;
    };
    sprite.tagElapsedMs += step;
    // a large step (a high speed, a long frame) goes round whole loops at once, or ends the
    // tag, instead of stepping frame by frame: the loop below runs at most one pass
    double loop = 0.0, rest = 0.0;
    for (size_t i = 0; i < sprite.tagFrames.size(); i++){
        loop += durationOf(i);
        if (i >= sprite.tagStep) rest += durationOf(i);
    }
    if (sprite.tagLoop && loop > 0.0 && sprite.tagElapsedMs >= loop){
        sprite.tagElapsedMs = std::fmod(sprite.tagElapsedMs, loop);
    }else if (!sprite.tagLoop && sprite.tagElapsedMs >= rest){
        sprite.tagStep = sprite.tagFrames.size() - 1;
        sprite.tagPlaying = false;
        sprite.tagElapsedMs = 0.0;
        sprite.frame = sprite.tagFrames[sprite.tagStep];
        return;
    }
    while (true){
        const double duration = durationOf(sprite.tagStep);
        if (sprite.tagElapsedMs < duration){
            break;
        }
        if (sprite.tagStep + 1 >= sprite.tagFrames.size()){
            if (!sprite.tagLoop){
                // finished: the last frame stays
                sprite.tagPlaying = false;
                sprite.tagElapsedMs = 0.0;
                break;
            }
            sprite.tagStep = 0;
        }else{
            sprite.tagStep++;
        }
        sprite.tagElapsedMs -= duration;
        if (duration <= 0.0){
            break;
        }
    }
    sprite.frame = sprite.tagFrames[sprite.tagStep];
}

bool MeshSystem::updateSheetSprite(SpriteComponent& sprite, MeshComponent& mesh){
    // the sheet: loaded once, and again when the pool has a newer version (a reimport); the
    // previous one stays drawn until the new one is there (no flicker)
    // another sheet (the Inspector, a script, a drop): its frames from now on
    if (sprite.sheetData && sprite.loadedSheet != sprite.sheet.path){
        sprite.sheetData.reset();
        sprite.builtFrame = -1;
    }
    const uint64_t revision = SpriteSheetPool::revision();
    if (!sprite.sheetData || sprite.sheetRevision != revision){
        sprite.sheetRevision = revision;
        const uint64_t generation = SpriteSheetPool::generation(sprite.sheet.path);
        if (!sprite.sheetData || generation != sprite.sheetGeneration){
            std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(sprite.sheet.path);
            if (sheet){
                sprite.sheetData = std::move(sheet);
                sprite.loadedSheet = sprite.sheet.path;
                sprite.sheetGeneration = SpriteSheetPool::generation(sprite.sheet.path);
                sprite.resolveFrameName = true;
                sprite.needUpdateSprite = true;
            }
        }
    }
    if (!sprite.sheetData || sprite.sheetData->frames.empty()){
        return false;
    }
    const SpriteSheetData& sheet = *sprite.sheetData;
    const int frameCount = (int)sheet.frames.size();
    // the frame by its saved name (a renamed file or a reordered atlas keeps the frame), else by index
    if (sprite.resolveFrameName){
        sprite.resolveFrameName = false;
        if (!sprite.frameName.empty()){
            const int named = sheet.findFrame(sprite.frameName);
            if (named >= 0){
                sprite.frame = named;
            }else if (sprite.frame < 0 || sprite.frame >= frameCount){
                Log::warn("Sprite sheet %s has no frame '%s' and %d frames: showing frame %d",
                          sprite.sheet.path.c_str(), sprite.frameName.c_str(), frameCount, std::clamp(sprite.frame, 0, frameCount - 1));
            }
        }
    }
    sprite.frame = std::clamp(sprite.frame, 0, frameCount - 1);

    const SpriteFrame& frame = sheet.frames[(size_t)sprite.frame];
    // Sliced (S45 R6): a bordered frame as its 9-slice; the lattice's grid does not apply to it
    const bool sliced = sprite.drawMode == SpriteDrawMode::SLICED && spriteFrameHasBorder(frame);
    const int grid = sliced ? 1 : requestedSpriteGrid(mesh);
    const int previousGrid = sprite.builtSliced ? 1 : builtSpriteGrid(mesh);
    if (!sprite.needUpdateSprite && sprite.frame == sprite.builtFrame && previousGrid == grid && sprite.builtSliced == sliced){
        return true;
    }

    if (frame.page >= sheet.pages.size()){
        return false;
    }
    const SpritePage& page = sheet.pages[frame.page];
    Texture& texture = mesh.submeshes[0].material.baseColorTexture;
    // a frame on the page already bound needs no path comparison (it copies the path: S45 R26's
    // zero allocations per frame change)
    const bool samePage = !sprite.needUpdateSprite && sprite.builtFrame >= 0 && sprite.builtFrame < frameCount &&
                          sheet.frames[(size_t)sprite.builtFrame].page == frame.page;
    if (!samePage && texture.getPath() != page.texture){
        // the page as the base colour texture; the texture keeps the sprite's sampler settings
        texture.setAlphaBorderAuto(true);
        texture.setPath(page.texture);
        mesh.submeshes[0].needUpdateTexture = true;
    }

    CameraComponent& camera = scene->getComponent<CameraComponent>(scene->getCamera());
    if (sprite.automaticFlipY){
        changeFlipY(sprite.flipY, camera, mesh);
    }
    SpriteFrameGeometryInput input;
    input.width = sprite.width;
    input.height = sprite.height;
    input.pivot = sprite.pivotPreset;
    // the Sprite's own Pixels Per Unit (S41) over its sheet's (the import's, else the project's)
    input.pixelsPerUnit = sprite.pixelsPerUnit > 0.0f ? sprite.pixelsPerUnit : sheet.pixelsPerUnit;
    input.pixelRatio = sheet.pixelRatio;
    input.uiCamera = camera.type == CameraType::CAMERA_UI;
    input.flipY = sprite.flipY;
    input.pageWidth = page.width;
    input.pageHeight = page.height;
    if (sliced || sprite.builtSliced){
        // a 9-slice is rebuilt on every frame change (its lattice depends on the frame's border);
        // back to Simple, the buffer goes back to the quad's size
        const bool wasSliced = sprite.builtSliced;
        prepareSpriteMesh(mesh);
        if (sliced){
            const SpriteSlicedMesh slicedMesh = spriteFrameSliced(frame, input);
            mesh.submeshes[0].textureRect = slicedMesh.textureRect;
            buildSpriteSliced(sprite, mesh, slicedMesh);
        }else{
            const SpriteFrameQuad quad = spriteFrameQuad(frame, input);
            mesh.submeshes[0].textureRect = quad.textureRect;
            buildSpriteGrid(sprite, mesh, (unsigned int)page.width, (unsigned int)page.height, quad.x0, quad.y0, quad.x1, quad.y1, grid);
        }
        mesh.submeshes[0].hasTextureRect = true;
        sprite.builtSliced = sliced;
        sprite.frameName = frame.name;
        sprite.needUpdateFrameRect = false;
        if (mesh.loaded){
            // a GPU buffer keeps the size it was created with: 16 vertices and 4 differ
            if (wasSliced != sliced || (previousGrid != 0 && previousGrid != grid)){
                mesh.needReload = true;
            }else{
                mesh.needUpdateBuffer = true;
            }
        }
        sprite.builtFrame = sprite.frame;
        sprite.needUpdateSprite = false;
        return true;
    }

    const SpriteFrameQuad quad = spriteFrameQuad(frame, input);

    // a frame of another trim on the grid built: the buffer keeps its layout and its indices
    const bool geometryOnly = !sprite.needUpdateSprite && sprite.builtFrame >= 0 && sprite.builtFrame < frameCount && previousGrid == grid;
    bool rebuild = !geometryOnly;
    if (!rebuild){
        // frames with the same trim geometry only swap the texture rect
        rebuild = !spriteFrameQuad(sheet.frames[(size_t)sprite.builtFrame], input).sameGeometry(quad);
    }
    mesh.submeshes[0].textureRect = quad.textureRect;
    mesh.submeshes[0].hasTextureRect = true;
    sprite.frameName = frame.name;
    sprite.needUpdateFrameRect = false;
    if (rebuild){
        if (geometryOnly){
            mesh.buffer.clear();
        }else{
            prepareSpriteMesh(mesh);
        }
        buildSpriteGrid(sprite, mesh, (unsigned int)page.width, (unsigned int)page.height, quad.x0, quad.y0, quad.x1, quad.y1, grid, geometryOnly);
        if (mesh.loaded){
            // a GPU buffer keeps the size it was created with: another grid reloads the mesh
            if (previousGrid != 0 && previousGrid != grid){
                mesh.needReload = true;
            }else{
                mesh.needUpdateBuffer = true;
            }
        }
    }
    sprite.builtFrame = sprite.frame;
    sprite.needUpdateSprite = false;
    return true;
}

// S45 R8: a legacy sprite whose texture an atlas in use packs draws its inline rect from the page
// (translated, clipped to the member's trim); the saved texture and rect stay the source's
// (SpriteInlineAtlas). Rechecked when the pool moves on, the rect or the texture changes; a legacy
// sprite of a project without atlases only compares the pool's revision.
bool MeshSystem::updateInlineAtlas(SpriteComponent& sprite, MeshComponent& mesh){
    Submesh& submesh = mesh.submeshes[0];
    Texture& texture = submesh.material.baseColorTexture;
    const uint64_t revision = SpriteSheetPool::revision();
    SpriteInlineAtlasRef state = sprite.inlineAtlas;
    const bool onPage = state && !state->page.empty() && texture.getPath() == state->page;
    const bool onSource = state && state->page.empty() && texture.getPath() == state->source;

    std::string source;
    Rect sourceRect = submesh.textureRect;
    Rect pending;
    bool hasPending = false;
    if (sprite.needUpdateFrameRect && !sprite.pendingFrameRect.isNormalized()){
        pending = sprite.pendingFrameRect;
        hasPending = true;
    }
    if (state && (onPage || onSource)){
        source = state->source;
        Rect current = submesh.textureRect;
        Rect mapped = onPage ? state->pageRect : state->sourceRect;
        // someone wrote a frame of the source (setFrame, a clip, the Inspector): map it
        if (current != mapped) sourceRect = current;
        else sourceRect = state->sourceRect;
        Rect stored = state->sourceRect;
        if (!hasPending && !(sourceRect != stored) && revision == state->revision && !sprite.needUpdateSprite){
            return onPage;
        }
    }else{
        if (state){
            // another texture was set: it is the source now (the page's rect means nothing for it)
            Rect current = submesh.textureRect;
            if (!state->page.empty() && !(current != state->pageRect)) sourceRect = state->sourceRect;
            sprite.inlineAtlas.reset();
            state.reset();
        }else if (static_cast<uint32_t>(revision) == sprite.inlineAtlasChecked && !sprite.needUpdateSprite && !submesh.needUpdateTexture && !hasPending){
            return false;
        }
        source = texture.getPath();
    }
    sprite.inlineAtlasChecked = static_cast<uint32_t>(revision);

    // back to the source texture, as it saves
    auto restore = [&](){
        if (onPage){
            texture.setPath(source);
            submesh.needUpdateTexture = true;
            submesh.textureRect = sourceRect;
            sprite.needUpdateSprite = true;
        }
    };
    if (source.empty() || !SpriteSheetSources::isPacked(source)){
        restore();
        sprite.inlineAtlas.reset();
        return false;
    }
    int sourceW = 0, sourceH = 0;
    if (state && state->source == source && state->sourceWidth > 0){
        // a frame change of a mapped sprite reads no file
        sourceW = state->sourceWidth;
        sourceH = state->sourceHeight;
    }else if (!SpriteSheetSources::textureSize(source, sourceW, sourceH) || sourceW <= 0 || sourceH <= 0){
        restore();
        sprite.inlineAtlas.reset();
        return false;
    }
    Rect pixels = hasPending ? pending : (sourceRect.isNormalized() ? Rect(sourceRect.getX() * sourceW, sourceRect.getY() * sourceH,
                                                                         sourceRect.getWidth() * sourceW, sourceRect.getHeight() * sourceH)
                                                                    : sourceRect);
    if (hasPending){
        sourceRect = normalizeTileRect(pending, (unsigned int)sourceW, (unsigned int)sourceH);
        sprite.needUpdateFrameRect = false;
    }
    SpriteFrame frame;
    std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::find(source);
    // mirrored frames (a negative size) and rects across packed frames draw from the source
    const bool mirrored = sourceRect.getWidth() < 0.0f || sourceRect.getHeight() < 0.0f;
    CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera());
    bool mapped = !mirrored && camera && sheet && SpriteSheetSources::mapInlineRect(source, pixels, frame) && frame.page < sheet->pages.size();
    // textureScaleFactor: createSprite samples the frame `cut` texels in from each edge; drawn from
    // the page, the frame is that inner part (clipped to the packed rect), with no UV cut of its own
    const float cut = sprite.textureScaleFactor;
    if (mapped && cut > 0.0f){
        Rect packed = frame.rect;
        const float innerW = frame.sourceSize.x - 2.0f * cut, innerH = frame.sourceSize.y - 2.0f * cut;
        const float x0 = std::max(frame.offset.x, cut), y0 = std::max(frame.offset.y, cut);
        const float x1 = std::min(frame.offset.x + packed.getWidth(), frame.sourceSize.x - cut);
        const float y1 = std::min(frame.offset.y + packed.getHeight(), frame.sourceSize.y - cut);
        if (innerW <= 0.0f || innerH <= 0.0f || x1 <= x0 || y1 <= y0){
            mapped = false;
        }else{
            frame.rect = Rect(packed.getX() + (x0 - frame.offset.x), packed.getY() + (y0 - frame.offset.y), x1 - x0, y1 - y0);
            frame.offset = Vector2(x0 - cut, y0 - cut);
            frame.sourceSize = Vector2(innerW, innerH);
        }
    }
    if (!mapped){
        restore();
        auto* unmapped = new SpriteInlineAtlas();
        unmapped->source = source;
        unmapped->sourceRect = sourceRect;
        unmapped->revision = revision;
        sprite.inlineAtlas = SpriteInlineAtlasRef(unmapped);
        if (onPage) return false;
        submesh.textureRect = sourceRect;
        return false;
    }
    const SpritePage& page = sheet->pages[frame.page];
    if (sprite.width == 0 && sprite.height == 0){
        sprite.width = (unsigned int)sourceW;
        sprite.height = (unsigned int)sourceH;
    }
    if (texture.getPath() != page.texture){
        texture.setAlphaBorderAuto(true);
        texture.setPath(page.texture);
        submesh.needUpdateTexture = true;
    }
    if (sprite.automaticFlipY){
        changeFlipY(sprite.flipY, *camera, mesh);
    }
    SpriteFrameGeometryInput input;
    input.width = sprite.width;
    input.height = sprite.height;
    // the quad in world units, as createSprite makes it (S41: its Pixels Per Unit, else the project's)
    input.pixelsPerUnit = resolvePixelsPerUnit(sprite.pixelsPerUnit);
    // inline frames carry no pivot: From Frame is the centre (createSprite's)
    input.pivot = sprite.pivotPreset;
    input.uiCamera = camera->type == CameraType::CAMERA_UI;
    input.flipY = sprite.flipY;
    input.pageWidth = page.width;
    input.pageHeight = page.height;
    const SpriteFrameQuad quad = spriteFrameQuad(frame, input);
    const int grid = requestedSpriteGrid(mesh);
    const int previousGrid = sprite.builtSliced ? 0 : builtSpriteGrid(mesh);
    submesh.textureRect = quad.textureRect;
    submesh.hasTextureRect = true;
    prepareSpriteMesh(mesh);
    // (a page size of 0: no UV cut, the frame above is already the cut one)
    buildSpriteGrid(sprite, mesh, cut > 0.0f ? 0u : (unsigned int)page.width, cut > 0.0f ? 0u : (unsigned int)page.height,
                    quad.x0, quad.y0, quad.x1, quad.y1, grid);
    if (mesh.loaded){
        if (sprite.builtSliced || (previousGrid != 0 && previousGrid != grid)){
            mesh.needReload = true;
        }else{
            mesh.needUpdateBuffer = true;
        }
    }
    sprite.builtSliced = false;
    auto* atlasState = new SpriteInlineAtlas();
    atlasState->source = source;
    atlasState->sourceRect = sourceRect;
    atlasState->page = page.texture;
    atlasState->pageRect = quad.textureRect;
    atlasState->sourceWidth = sourceW;
    atlasState->sourceHeight = sourceH;
    atlasState->revision = revision;
    sprite.inlineAtlas = SpriteInlineAtlasRef(atlasState);
    sprite.needUpdateSprite = false;
    return true;
}

bool MeshSystem::createOrUpdateSprite(SpriteComponent& sprite, MeshComponent& mesh){
    if (!sprite.sheet.empty()){
        return updateSheetSprite(sprite, mesh);
    }
    if (updateInlineAtlas(sprite, mesh)){
        return true;
    }

    // a lattice started or stopped deforming it, or asks for another grid
    if (!sprite.needUpdateSprite){
        const int built = builtSpriteGrid(mesh);
        if (built != 0 && built != requestedSpriteGrid(mesh)){
            sprite.needUpdateSprite = true;
        }
    }

    if (sprite.needUpdateSprite){
        // no camera yet (a scene drawn before it was loaded): built once it has one
        CameraComponent* cameraPtr = scene->findComponent<CameraComponent>(scene->getCamera());
        if (!cameraPtr){
            return false;
        }
        CameraComponent& camera = *cameraPtr;
        if (sprite.automaticFlipY){
            changeFlipY(sprite.flipY, camera, mesh);
        }

        if (createSprite(sprite, mesh, camera)){
            sprite.needUpdateSprite = false;
            sprite.builtSliced = false;
        }else{
            return false;
        }
    }

    if (sprite.needUpdateFrameRect){
        setSpriteFrameRect(mesh, sprite, sprite.pendingFrameRect);
    }

    return true;
}

bool MeshSystem::createOrUpdateTerrain(TerrainComponent& terrain, MeshComponent& mesh, Transform& transform){
    if (terrain.needUpdateTerrain){

        if (scene->getCamera() == NULL_ENTITY){
            Log::error("Cannot create terrain without defined camera in scene");
            terrain.needUpdateTerrain = false;
            return false;
        }

        // Only the odd vertices of a grid morph, so the halfRes grid (resolution/2) needs
        // an even number of segments as well to keep its border vertices in place.
        float gridResolution = std::max(4.0f, std::round(terrain.resolution / 4) * 4);
        if (terrain.resolution != gridResolution){
            Log::warn("Terrain resolution %d is not a multiple of 4, using %d", (int)terrain.resolution, (int)gridResolution);
            terrain.resolution = gridResolution;
        }

        if (MAX_TERRAINGRID < (terrain.rootGridSize*terrain.rootGridSize)){
            // Tear down rather than only skip: rootGridSize has already moved past the
            // grid[] cap, so a previously built tree left live would make updateTerrain
            // loop rootGridSize^2 times over the fixed MAX_TERRAINGRID-slot grid[] and
            // read out of bounds.
            Log::error("Cannot create full terrain, increase MAX_TERRAINGRID to %u", (terrain.rootGridSize*terrain.rootGridSize));
            resetTerrainToUnbuilt(terrain, mesh);
            return false;
        }

        // Reject an exponential node explosion instead of crashing: a large "levels"
        // makes the quadtree node vector request trillions of elements (bad_alloc).
        // The levels check runs first so getTerrainGridArraySize's 4^i math can't
        // overflow before the node-count check sees it.
        if (terrain.rootGridSize < 1 || terrain.levels < 1 || terrain.levels > MAX_TERRAIN_LEVELS ||
            getTerrainGridArraySize(terrain.rootGridSize, terrain.levels) > MAX_TERRAIN_NODES){
            // Tear the terrain down rather than only skipping the rebuild: levels/ranges
            // have already moved to the new (over-budget) depth, so keeping the old tree
            // live would make updateTerrain walk past its leaves and index ranges[] and
            // childs[] out of bounds. resetTerrainToUnbuilt gates updateTerrain off.
            Log::error("Terrain LOD too high (rootGridSize=%d, levels=%d): would exceed the %u-node budget. Lower Levels (or Root Grid Size).",
                       terrain.rootGridSize, terrain.levels, MAX_TERRAIN_NODES);
            resetTerrainToUnbuilt(terrain, mesh);
            return false;
        }

        if (terrain.heightMap.empty() && !terrain.world){
            resetTerrainToUnbuilt(terrain, mesh);
            return false;
        }

        if (createTerrain(terrain, mesh)){
            terrain.needUpdateTerrain = false;

            transform.needUpdate = true;
        }else{
            return false;
        }
    }

    return true;
}

bool MeshSystem::createOrUpdateMeshPolygon(MeshPolygonComponent& polygon, MeshComponent& mesh){
    if (polygon.needUpdatePolygon){
        if (polygon.automaticFlipY){
            CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera());
            if (!camera){
                return false;
            }
            changeFlipY(polygon.flipY, *camera, mesh);
        }

        if (createMeshPolygon(polygon, mesh)){
            polygon.needUpdatePolygon = false;
        }else{
            return false;
        }
    }

    return true;
}

bool MeshSystem::createOrUpdateTilemap(TilemapComponent& tilemap, MeshComponent& mesh){
    if (tilemap.needUpdateTilemap){
        if (tilemap.automaticFlipY){
            CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera());
            if (!camera){
                return false;
            }
            changeFlipY(tilemap.flipY, *camera, mesh);
        }

        if (createTilemap(tilemap, mesh)){
            tilemap.needUpdateTilemap = false;
        }else{
            return false;
        }
    }

    return true;
}

bool MeshSystem::createOrUpdateModel(Entity entity, ModelComponent& model, MeshComponent& mesh){
    if (model.needUpdateModel){
        if (!model.filename.empty()){
            if (getModelFilenameKey(model.filename) == model.loadedFilename &&
                    model.mergeStaticMeshes == model.loadedMergeStaticMeshes){
                model.needUpdateModel = false;
                return true;
            }

            // Retrying a file that cannot be read re-parses it every frame, forever.
            if (failedModelLoads.count(model.filename)){
                model.needUpdateModel = false;
                return false;
            }

            std::string ext = FileData::getFilePathExtension(model.filename);
            // Entities that exist (a decoded scene) are kept, with their layout. Animation entities
            // count only when the model makes them (animationEntities): one that does not keeps its
            // bones and mesh nodes, so it still skips, and a merged one rebuilds without them.
            bool skipEntities = !model.filename.empty() && (!model.bonesIdMapping.empty() || !model.animations.empty() || !model.meshNodesMapping.empty());
            bool asyncLoading = Engine::isAsyncLoading();
            bool ret = false;
            if (ext == "obj"){
                ret = loadOBJ(entity, model.filename, asyncLoading);
            }else{
                ret = loadGLTF(entity, model.filename, asyncLoading, skipEntities, false);
            }

            if (ret){
                model.needUpdateModel = false;
            }else{
                // A load still running also returns false, its key stays in the pending map.
                if (!asyncLoading || !isAsyncModelLoadPending(model.filename)){
                    Log::error("Model load failed, not retrying: %s", model.filename.c_str());
                    failedModelLoads.insert(model.filename);
                    model.needUpdateModel = false;
                }
                return false;
            }
        }else{
            model.loadedFilename.clear();
            model.loadedMergeStaticMeshes = false;
            model.needUpdateModel = false;
        }
    }

    return true;
}

void MeshSystem::load(){
    update(0);
}

void MeshSystem::destroy(){
    TerrainEditEvents::forgetScene(scene);
}

void MeshSystem::update(double dt){
    // before the pause: a paused scene's abandoned load closes its build too
    collectUnclaimedAsyncModelLoads();

    if (paused) {
        return;
    }

    // the project's Pixels Per Unit changed: what takes it builds again (S41)
    const bool pixelsPerUnitChanged = Engine::getPixelsPerUnit() != builtPixelsPerUnit;
    builtPixelsPerUnit = Engine::getPixelsPerUnit();

    auto sprites = scene->getComponentArray<SpriteComponent>();
    for (int i = 0; i < sprites->size(); i++){
        SpriteComponent& sprite = sprites->getComponentFromIndex(i);

        Entity entity = sprites->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (pixelsPerUnitChanged && sprite.pixelsPerUnit <= 0.0f)
            sprite.needUpdateSprite = true;

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            if (sprite.tagPlaying){
                stepSpriteTag(sprite, dt);
            }
            createOrUpdateSprite(sprite, mesh);
        }
    }

    // The resolve creates and destroys entities, which can compact the terrain array.

    // a Pixel Perfect Camera's tilemaps round their tile edges to the texel grid (S41 R13)
    RenderSystem* renderSystem = scene->getSystem<RenderSystem>().get();
    const bool roundTileEdges = renderSystem && renderSystem->hasPixelSnapVariants();
    auto tilemaps = scene->getComponentArray<TilemapComponent>();
    for (int i = 0; i < tilemaps->size(); i++){
        TilemapComponent& tilemap = tilemaps->getComponentFromIndex(i);

        Entity entity = tilemaps->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (tilemap.roundTileEdges != roundTileEdges || (pixelsPerUnitChanged && tilemap.pixelsPerUnit <= 0.0f)){
            tilemap.roundTileEdges = roundTileEdges;
            tilemap.needUpdateTilemap = true;
        }

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            createOrUpdateTilemap(tilemap, mesh);
        }
    }

    auto polygons = scene->getComponentArray<MeshPolygonComponent>();
    for (int i = 0; i < polygons->size(); i++){
        MeshPolygonComponent& polygon = polygons->getComponentFromIndex(i);

        Entity entity = polygons->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            createOrUpdateMeshPolygon(polygon, mesh);
        }
    }

    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (int i = 0; i < terrains->size(); i++){
        TerrainComponent& terrain = terrains->getComponentFromIndex(i);

        Entity entity = terrains->getEntity(i);
        Signature signature = scene->getSignature(entity);

#if !defined(JASPERENGINE_EDITOR) && JASPERENGINE_TERRAIN_EDIT
        // T1 R77, R78: a game's terrain with Allow Runtime Edits draws from copies of its maps,
        // made at load and kept readable (the editor makes them when Play starts)
        // (once: both outcomes leave an edit state)
        if (terrain.allowRuntimeEdits && !terrain.editState){
            // R46: a map too large for this device stays shared; writes refuse with the reason
            const std::string refusal = terrainRuntimeEditRefusal(terrain);
            if (refusal.empty()){
                detachTerrainMapsForRuntime(terrain, entity);
            }else{
                terrain.editState = std::make_shared<TerrainEditState>();
                Log::warn("Terrain %u: Allow Runtime Edits is off on this device: %s", entity, refusal.c_str());
            }
        }
#endif

        if (signature.test(scene->getComponentId<MeshComponent>()) && signature.test(scene->getComponentId<Transform>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            createOrUpdateTerrain(terrain, mesh, transform);
            updateTerrainAutoRanges(terrain);
        }
#if JASPERENGINE_TERRAIN_EDIT
        if (terrain.editState){
            flushTerrainEdit(terrain, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }
#endif
    }
    // T1 R42, DC10: a frame's edits reach their subscribers once, by terrain and map
    if (TerrainEditEvents::hasPending()){
        TerrainEditEvents::flush(scene);
    }

    auto models = scene->getComponentArray<ModelComponent>();
    for (int i = 0; i < models->size(); i++){
        ModelComponent& model = models->getComponentFromIndex(i);

        Entity entity = models->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            createOrUpdateModel(entity, model, mesh);
        }
    }

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);

        if (mesh.aabb == AABB::ZERO){
            calculateMeshAABB(mesh);
        }

        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            if (!mesh.submeshes[s].hasTextureRect && usesTextureRect(mesh.submeshes[s])){
                mesh.submeshes[s].hasTextureRect = true;
                mesh.needReload = true;
            }
        }
    }

}

void MeshSystem::draw(){

}

void MeshSystem::onComponentAdded(Entity entity, ComponentId componentId) {
    if (componentId == scene->getComponentId<InstancedMeshComponent>()) {
        InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);
        if (instmesh.buffer.getAttributes().empty()) {
            instmesh.buffer.clear();
            instmesh.buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL1, 4, 0, true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL2, 4, 4 * sizeof(float), true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL3, 4, 8 * sizeof(float), true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL4, 4, 12 * sizeof(float), true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCECOLOR, 4, 16 * sizeof(float), true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCETEXTURERECT, 4, 20 * sizeof(float), true);
            instmesh.buffer.setStride(24 * sizeof(float));
            instmesh.buffer.setRenderAttributes(true);
            instmesh.buffer.setInstanceBuffer(true);
            instmesh.buffer.setUsage(BufferUsage::STREAM);
        }
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            if (mesh->loaded)
                mesh->needReload = true;
        }
    }
}

void MeshSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    if (componentId == scene->getComponentId<ModelComponent>()) {
        ModelComponent& model = scene->getComponent<ModelComponent>(entity);
        // The meshes the model loaded stay, with its geometry: their external buffers point
        // into the model's file data, which goes with it, so they keep copies of their own
        // (drawn and saved as the meshes they are now)
        auto keepGeometry = [this](Entity meshEntity){
            if (MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity)){
                for (unsigned int i = 0; i < mesh->numExternalBuffers; i++){
                    mesh->eBuffers[i].copyToOwnData();
                }
            }
        };
        keepGeometry(entity);
        for (const auto& node : model.meshNodesMapping){
            keepGeometry(node.second);
        }
        destroyModel(model);
    }
    if (componentId == scene->getComponentId<InstancedMeshComponent>()) {
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            mesh->aabb = mesh->verticesAABB;
            mesh->needUpdateAABB = true;
            if (mesh->loaded) mesh->needReload = true;
        }
    }
}
