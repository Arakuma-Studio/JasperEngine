// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/GlbBuilder.h"

#include "util/FileUtils.h"
#include "util/Util.h"
#include "Engine.h"

#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace jasperengine::editor {

namespace {

std::string toString(const ufbx_string& text) {
    return std::string(text.data, text.length);
}

bool readFile(const fs::path& path, std::vector<unsigned char>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    file.seekg(0);
    return size == 0 || static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

std::string encodeUri(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string sniffMimeType(const std::vector<unsigned char>& bytes) {
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
        return "image/png";
    }
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return "image/jpeg";
    }
    return {};
}

void appendToVector(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(context);
    const auto* bytes = static_cast<const unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stemOf(const std::string& name) {
    return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(name).stem());
}

void appendMatrix(std::vector<float>& out, const ufbx_matrix& m) {
    for (int c = 0; c < 4; c++) {
        out.push_back(static_cast<float>(m.cols[c].x));
        out.push_back(static_cast<float>(m.cols[c].y));
        out.push_back(static_cast<float>(m.cols[c].z));
        out.push_back(c == 3 ? 1.0f : 0.0f);
    }
}

const ufbx_texture* fileTexture(const ufbx_material_map& map) {
    const ufbx_texture* texture = map.texture_enabled ? map.texture : nullptr;
    if (!texture) {
        return nullptr;
    }
    if (texture->type == UFBX_TEXTURE_FILE) {
        return texture->has_file ? texture : nullptr;
    }
    // A layered texture uses its first file
    for (const ufbx_texture* file : texture->file_textures) {
        if (file->has_file) {
            return file;
        }
    }
    return nullptr;
}

std::string textureName(const ufbx_texture_file& file) {
    std::string name = baseName(toString(file.relative_filename));
    if (name.empty()) {
        name = baseName(toString(file.filename));
    }
    return name.empty() ? "texture" + std::to_string(file.index) : name;
}

// The UV set a texture names on this mesh; the first one when it names none
size_t uvSetOf(const ufbx_texture* texture, const ufbx_mesh* mesh) {
    if (!texture || texture->uv_set.length == 0) {
        return 0;
    }
    const std::string set = toString(texture->uv_set);
    for (size_t i = 0; i < mesh->uv_sets.count; i++) {
        if (toString(mesh->uv_sets.data[i].name) == set) {
            return i;
        }
    }
    return 0;
}

// The four heaviest influences, normalized (ufbx sorts them by weight)
void fillWeights(const ufbx_skin_deformer* skin, const std::vector<int>& jointOfCluster, uint32_t vertex,
                 int fallbackJoint, uint16_t* joints, float* weights) {
    int filled = 0;
    float total = 0.0f;
    if (vertex < skin->vertices.count) {
        const ufbx_skin_vertex& skinVertex = skin->vertices.data[vertex];
        for (uint32_t w = 0; w < skinVertex.num_weights && filled < 4; w++) {
            const ufbx_skin_weight& weight = skin->weights.data[skinVertex.weight_begin + w];
            if (weight.weight <= 0.0 || weight.cluster_index >= jointOfCluster.size() || jointOfCluster[weight.cluster_index] < 0) {
                continue;
            }
            joints[filled] = static_cast<uint16_t>(jointOfCluster[weight.cluster_index]);
            weights[filled] = static_cast<float>(weight.weight);
            total += weights[filled];
            filled++;
        }
    }
    if (total > 0.0f) {
        for (int i = 0; i < filled; i++) {
            weights[i] /= total;
        }
    } else if (fallbackJoint >= 0) {
        joints[0] = static_cast<uint16_t>(fallbackJoint);
        weights[0] = 1.0f;
    }
}

float sampleWeight(const ufbx_baked_prop* track, double time, float fallback) {
    if (!track || track->keys.count == 0) {
        return fallback;
    }
    const ufbx_baked_vec3* keys = track->keys.data;
    const size_t count = track->keys.count;
    if (time <= keys[0].time) return static_cast<float>(keys[0].value.x / 100.0);
    if (time >= keys[count - 1].time) return static_cast<float>(keys[count - 1].value.x / 100.0);
    for (size_t i = 1; i < count; i++) {
        if (time <= keys[i].time) {
            const double span = keys[i].time - keys[i - 1].time;
            const double t = span > 0.0 ? (time - keys[i - 1].time) / span : 0.0;
            return static_cast<float>((keys[i - 1].value.x + (keys[i].value.x - keys[i - 1].value.x) * t) / 100.0);
        }
    }
    return static_cast<float>(keys[count - 1].value.x / 100.0);
}

struct Vertex {
    float position[3];
    float normal[3];
    float uv0[2];
    float uv1[2];
    float color[4];
    uint32_t vertex;
};

} // namespace

struct GlbBuilder::Pixels {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

GlbBuilder::GlbBuilder(const ufbx_scene* scene, const fs::path& fbxPath, const fs::path& assetsRoot)
    : scene(scene), fbxPath(fbxPath), assetsRoot(assetsRoot) {
    model.buffers.resize(1);
}

bool GlbBuilder::build(std::string& error) {
    for (const ufbx_mesh* mesh : scene->meshes) {
        buildGeometry(mesh);
    }
    if (geometries.empty()) {
        error = "the file has no triangle meshes";
        return false;
    }

    buildNodes();
    buildAnimations();

    tinygltf::Scene root;
    root.nodes.push_back(static_cast<int>(scene->root_node->typed_id));
    model.scenes.push_back(root);
    model.defaultScene = 0;
    return true;
}

std::vector<unsigned char> GlbBuilder::write(const tinygltf::Value& extras) {
    model.asset.version = "2.0";
    model.asset.generator = "JasperEngine FBX importer (ufbx)";
    model.asset.extras = extras;

    tinygltf::TinyGLTF writer;
    // The default image writer cuts a linked image's URI down to its file name
    writer.SetImageWriter([](const std::string*, const std::string*, const tinygltf::Image* image, bool,
                             const tinygltf::FsCallbacks*, const tinygltf::URICallbacks*, std::string* uri, void*) {
        *uri = image->uri;
        return true;
    }, nullptr);

    std::ostringstream stream;
    if (!writer.WriteGltfSceneToStream(&model, stream, false, true)) {
        return {};
    }
    const std::string data = stream.str();
    return std::vector<unsigned char>(data.begin(), data.end());
}

const std::set<fs::path>& GlbBuilder::getDependencies() const {
    return dependencies;
}

bool GlbBuilder::isLinked(const fs::path& path) const {
    return linkedFiles.count(path) > 0;
}

std::vector<GlbBuilder::TextureLookup> GlbBuilder::getTextureLookups() const {
    std::vector<TextureLookup> lookups;
    for (const auto& entry : textureLookups) {
        lookups.push_back(entry.second);
    }
    return lookups;
}

const std::vector<std::string>& GlbBuilder::getWarnings() const {
    return warnings;
}

void GlbBuilder::warn(const std::string& message) {
    if (warned.insert(message).second) {
        warnings.push_back(message);
    }
}

int GlbBuilder::addBufferView(const void* data, size_t size, int target) {
    std::vector<unsigned char>& bin = model.buffers[0].data;
    while (bin.size() % 4 != 0) {
        bin.push_back(0);
    }
    tinygltf::BufferView view;
    view.buffer = 0;
    view.byteOffset = bin.size();
    view.byteLength = size;
    view.target = target;
    const auto* bytes = static_cast<const unsigned char*>(data);
    bin.insert(bin.end(), bytes, bytes + size);
    model.bufferViews.push_back(view);
    return static_cast<int>(model.bufferViews.size()) - 1;
}

int GlbBuilder::addAccessor(int view, int componentType, size_t count, int type) {
    tinygltf::Accessor accessor;
    accessor.bufferView = view;
    accessor.componentType = componentType;
    accessor.count = count;
    accessor.type = type;
    model.accessors.push_back(accessor);
    return static_cast<int>(model.accessors.size()) - 1;
}

int GlbBuilder::addFloatAccessor(const std::vector<float>& values, int type, int target, bool bounds) {
    const int components = tinygltf::GetNumComponentsInType(type);
    const size_t count = values.size() / components;
    const int view = addBufferView(values.data(), values.size() * sizeof(float), target);
    const int index = addAccessor(view, TINYGLTF_COMPONENT_TYPE_FLOAT, count, type);
    if (bounds && count > 0) {
        tinygltf::Accessor& accessor = model.accessors[index];
        accessor.minValues.assign(components, FLT_MAX);
        accessor.maxValues.assign(components, -FLT_MAX);
        for (size_t i = 0; i < count; i++) {
            for (int c = 0; c < components; c++) {
                accessor.minValues[c] = std::min<double>(accessor.minValues[c], values[i * components + c]);
                accessor.maxValues[c] = std::max<double>(accessor.maxValues[c], values[i * components + c]);
            }
        }
    }
    return index;
}

fs::path GlbBuilder::findTextureFile(const TextureRef& texture, const fs::path& fbxPath, const fs::path& assetsRoot) {
    std::error_code ec;
    auto existing = [&](const std::string& candidate, bool insideAssets) -> fs::path {
        if (candidate.empty()) {
            return {};
        }
        fs::path path = FileUtils::pathFromUtf8(candidate);
        if (path.is_relative()) {
            path = fbxPath.parent_path() / path;
        }
        path = path.lexically_normal();
        if ((!insideAssets || Util::isInsidePath(path, assetsRoot)) && fs::is_regular_file(path, ec)) {
            return path;
        }
        return {};
    };

    // A copy inside the assets wins over the artist's absolute path
    for (const std::string& candidate : {texture.filename, texture.relative, texture.absolute}) {
        const fs::path path = existing(candidate, true);
        if (!path.empty()) {
            return path;
        }
    }

    // Then search by name near the FBX
    std::string name = baseName(texture.relative);
    if (name.empty()) {
        name = baseName(texture.absolute);
    }
    if (name.empty()) {
        return {};
    }
    const fs::path fileName = FileUtils::pathFromUtf8(name);
    static const char* subdirs[] = {"", "textures", "Textures", "images", "Images", "materials", "Materials",
        "maps", "Maps", "tex", "Tex"};
    fs::path dir = fbxPath.parent_path().lexically_normal();
    while (true) {
        for (const char* subdir : subdirs) {
            const fs::path path = *subdir ? dir / subdir / fileName : dir / fileName;
            if (fs::is_regular_file(path, ec)) {
                return path.lexically_normal();
            }
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir || !Util::isInsidePath(parent, assetsRoot)) {
            break;
        }
        dir = parent;
    }

    for (const std::string& candidate : {texture.filename, texture.absolute}) {
        const fs::path path = existing(candidate, false);
        if (!path.empty()) {
            return path;
        }
    }
    return {};
}

// Embedded bytes, or the file on disk the FBX points at
bool GlbBuilder::resolveTexture(uint32_t fileIndex, std::vector<unsigned char>& bytes, fs::path& path) {
    if (fileIndex >= scene->texture_files.count) {
        return false;
    }
    const ufbx_texture_file& file = scene->texture_files.data[fileIndex];
    if (file.content.size > 0) {
        const auto* data = static_cast<const unsigned char*>(file.content.data);
        bytes.assign(data, data + file.content.size);
        return true;
    }
    const TextureRef texture = {toString(file.filename), toString(file.relative_filename), toString(file.absolute_filename)};
    path = findTextureFile(texture, fbxPath, assetsRoot);
    textureLookups.emplace(fileIndex, TextureLookup{texture, path});
    if (path.empty()) {
        warn("Texture not found: " + textureName(file));
        return false;
    }
    return true;
}

std::shared_ptr<GlbBuilder::Pixels> GlbBuilder::loadPixels(uint32_t fileIndex) {
    auto cached = pixelCache.find(fileIndex);
    if (cached != pixelCache.end()) {
        return cached->second;
    }

    std::shared_ptr<Pixels> pixels;
    std::vector<unsigned char> bytes;
    fs::path path;
    if (resolveTexture(fileIndex, bytes, path)) {
        if (!path.empty()) {
            readFile(path, bytes);
            dependencies.insert(path);
        }
        int width = 0;
        int height = 0;
        int components = 0;
        unsigned char* data = bytes.empty() ? nullptr :
            stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &components, 4);
        if (data) {
            pixels = std::make_shared<Pixels>();
            pixels->width = width;
            pixels->height = height;
            pixels->rgba.assign(data, data + static_cast<size_t>(width) * height * 4);
            stbi_image_free(data);
        } else {
            warn("Unsupported image format: " + textureName(scene->texture_files.data[fileIndex]));
        }
    }
    pixelCache[fileIndex] = pixels;
    return pixels;
}

int GlbBuilder::addEmbeddedImage(const std::vector<unsigned char>& bytes, const std::string& mimeType, const std::string& name) {
    tinygltf::Image image;
    image.name = name;
    image.mimeType = mimeType;
    image.bufferView = addBufferView(bytes.data(), bytes.size(), 0);
    model.images.push_back(image);
    return static_cast<int>(model.images.size()) - 1;
}

int GlbBuilder::addPngImage(const Pixels& pixels, const std::string& name) {
    std::vector<unsigned char> png;
    if (!stbi_write_png_to_func(appendToVector, &png, pixels.width, pixels.height, 4, pixels.rgba.data(), pixels.width * 4)) {
        warn("Could not encode image " + name);
        return -1;
    }
    return addEmbeddedImage(png, "image/png", name);
}

// A URI when the file sits in the assets, embedded otherwise
int GlbBuilder::imageForFile(uint32_t fileIndex) {
    auto cached = fileImages.find(fileIndex);
    if (cached != fileImages.end()) {
        return cached->second;
    }

    int image = -1;
    std::vector<unsigned char> bytes;
    fs::path path;
    if (resolveTexture(fileIndex, bytes, path)) {
        const std::string name = stemOf(textureName(scene->texture_files.data[fileIndex]));
        if (!path.empty() && Util::isInsidePath(path, assetsRoot)) {
            // Not embedded, but an edit must still reimport, also one that fixes an unreadable file
            dependencies.insert(path);
            std::vector<unsigned char> fileBytes;
            int width = 0;
            int height = 0;
            int components = 0;
            if (readFile(path, fileBytes) && stbi_info_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()),
                    &width, &height, &components)) {
                tinygltf::Image linked;
                linked.name = name;
                // The engine resolves it from the FBX, wherever the .glb is read from
                linked.uri = encodeUri(FileUtils::pathToGenericUtf8(path.lexically_relative(fbxPath.parent_path().lexically_normal())));
                model.images.push_back(linked);
                image = static_cast<int>(model.images.size()) - 1;
                linkedFiles.insert(path);
            } else {
                warn("Unsupported image format: " + FileUtils::pathToUtf8(path.filename()));
            }
        } else {
            if (!path.empty()) {
                readFile(path, bytes);
                dependencies.insert(path);
            }
            const std::string mimeType = sniffMimeType(bytes);
            if (!mimeType.empty()) {
                image = addEmbeddedImage(bytes, mimeType, name);
            } else {
                // glTF only carries PNG and JPEG
                std::shared_ptr<Pixels> pixels = loadPixels(fileIndex);
                if (pixels) {
                    image = addPngImage(*pixels, name);
                }
            }
        }
    }
    fileImages[fileIndex] = image;
    return image;
}

int GlbBuilder::textureFor(int image, const ufbx_texture* texture) {
    if (image < 0) {
        return -1;
    }
    const int wrapS = texture && texture->wrap_u == UFBX_WRAP_CLAMP ? TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE : TINYGLTF_TEXTURE_WRAP_REPEAT;
    const int wrapT = texture && texture->wrap_v == UFBX_WRAP_CLAMP ? TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE : TINYGLTF_TEXTURE_WRAP_REPEAT;
    if (texture && texture->has_uv_transform) {
        warn("Texture tiling/offset is not imported: " + toString(texture->name));
    }

    auto samplerIt = samplerIndices.find({wrapS, wrapT});
    int sampler;
    if (samplerIt == samplerIndices.end()) {
        tinygltf::Sampler wrap;
        wrap.wrapS = wrapS;
        wrap.wrapT = wrapT;
        model.samplers.push_back(wrap);
        sampler = static_cast<int>(model.samplers.size()) - 1;
        samplerIndices[{wrapS, wrapT}] = sampler;
    } else {
        sampler = samplerIt->second;
    }

    auto textureIt = textureIndices.find({image, sampler});
    if (textureIt != textureIndices.end()) {
        return textureIt->second;
    }
    tinygltf::Texture out;
    out.source = image;
    out.sampler = sampler;
    model.textures.push_back(out);
    const int index = static_cast<int>(model.textures.size()) - 1;
    textureIndices[{image, sampler}] = index;
    return index;
}

// FBX keeps opacity in its own map, glTF in the base color alpha
int GlbBuilder::combineOpacity(const ufbx_texture* base, const ufbx_texture* opacity, std::shared_ptr<Pixels>& pixels) {
    const std::string key = (base ? std::to_string(base->file_index) : "-") + ":" + std::to_string(opacity->file_index);
    auto cached = maskedImages.find(key);
    if (cached != maskedImages.end()) {
        pixels = cached->second.second;
        return cached->second.first;
    }

    std::shared_ptr<Pixels> color = base ? loadPixels(base->file_index) : nullptr;
    std::shared_ptr<Pixels> mask = loadPixels(opacity->file_index);
    if (!mask || (base && !color)) {
        return -1;
    }
    auto result = std::make_shared<Pixels>();
    if (color) {
        *result = *color;
    } else {
        result->width = mask->width;
        result->height = mask->height;
        result->rgba.assign(static_cast<size_t>(mask->width) * mask->height * 4, 255);
    }
    mask = resized(mask, result->width, result->height);
    if (!mask) {
        return -1;
    }
    const size_t count = result->rgba.size() / 4;
    for (size_t i = 0; i < count; i++) {
        result->rgba[i * 4 + 3] = static_cast<unsigned char>(result->rgba[i * 4 + 3] * mask->rgba[i * 4] / 255);
    }

    const std::string name = base ? textureName(scene->texture_files.data[base->file_index]) : "opacity";
    const int image = addPngImage(*result, stemOf(name) + "_alpha");
    maskedImages[key] = {image, result};
    pixels = result;
    return image;
}

// glTF reads roughness from G and metalness from B of one texture
int GlbBuilder::packMetallicRoughness(const ufbx_texture* roughness, const ufbx_texture* metalness, bool glossiness) {
    const std::string key = (roughness ? std::to_string(roughness->file_index) : "-") + ":" +
        (metalness ? std::to_string(metalness->file_index) : "-") + (glossiness ? ":g" : "");
    auto cached = packedImages.find(key);
    if (cached != packedImages.end()) {
        return cached->second;
    }

    std::shared_ptr<Pixels> rough = roughness ? loadPixels(roughness->file_index) : nullptr;
    std::shared_ptr<Pixels> metal = metalness ? loadPixels(metalness->file_index) : nullptr;
    if ((roughness && !rough) || (metalness && !metal)) {
        return -1;
    }
    const int width = std::max(rough ? rough->width : 0, metal ? metal->width : 0);
    const int height = std::max(rough ? rough->height : 0, metal ? metal->height : 0);
    rough = resized(rough, width, height);
    metal = resized(metal, width, height);
    if ((roughness && !rough) || (metalness && !metal)) {
        return -1;
    }

    Pixels packed;
    packed.width = width;
    packed.height = height;
    packed.rgba.assign(static_cast<size_t>(width) * height * 4, 255);
    const size_t count = packed.rgba.size() / 4;
    for (size_t i = 0; i < count; i++) {
        if (rough) {
            const unsigned char value = rough->rgba[i * 4];
            packed.rgba[i * 4 + 1] = glossiness ? static_cast<unsigned char>(255 - value) : value;
        }
        if (metal) {
            packed.rgba[i * 4 + 2] = metal->rgba[i * 4];
        }
    }
    const ufbx_texture* named = roughness ? roughness : metalness;
    const int image = addPngImage(packed, stemOf(textureName(scene->texture_files.data[named->file_index])) + "_mr");
    packedImages[key] = image;
    return image;
}

// The glTF texCoord a texture samples on this mesh
int GlbBuilder::texCoordFor(const ufbx_texture* texture, const ufbx_mesh* mesh) {
    const size_t set = uvSetOf(texture, mesh);
    if (set < 2) {
        return static_cast<int>(set);
    }
    warn("UV set '" + toString(texture->uv_set) + "' is not imported, its textures use the first set");
    return 0;
}

int GlbBuilder::materialFor(const ufbx_material* material, const ufbx_mesh* mesh) {
    const ufbx_material_pbr_maps& pbr = material->pbr;

    const ufbx_texture* baseTexture = fileTexture(pbr.base_color);
    const ufbx_texture* opacityTexture = fileTexture(pbr.opacity);
    if (!opacityTexture) {
        opacityTexture = fileTexture(material->fbx.transparency_color);
    }
    if (opacityTexture && baseTexture && opacityTexture->file_index == baseTexture->file_index) {
        opacityTexture = nullptr;
    }
    // ufbx moves roughness into the glossiness map when a material stores glossiness
    const bool glossiness = material->features.roughness_as_glossiness.enabled;
    const ufbx_texture* roughnessTexture = fileTexture(glossiness ? pbr.glossiness : pbr.roughness);
    const ufbx_texture* metalnessTexture = fileTexture(pbr.metalness);
    const ufbx_texture* normalTexture = fileTexture(pbr.normal_map);
    const ufbx_texture* occlusionTexture = fileTexture(pbr.ambient_occlusion);
    const ufbx_texture* emissionTexture = fileTexture(pbr.emission_color);

    // The mesh's UV sets decide what each map samples, so they are part of the key
    const std::vector<size_t> sets = {uvSetOf(baseTexture, mesh), uvSetOf(opacityTexture, mesh),
        uvSetOf(roughnessTexture, mesh), uvSetOf(metalnessTexture, mesh), uvSetOf(normalTexture, mesh),
        uvSetOf(occlusionTexture, mesh), uvSetOf(emissionTexture, mesh)};
    const auto key = std::make_pair(material, sets);
    auto cached = materialIndices.find(key);
    if (cached != materialIndices.end()) {
        return cached->second;
    }

    // Maps baked into one texture must share a UV set
    const std::string name = toString(material->name);
    if (baseTexture && opacityTexture && uvSetOf(baseTexture, mesh) != uvSetOf(opacityTexture, mesh)) {
        warn("Material '" + name + "': its opacity map uses another UV set than the base color and is not imported");
        opacityTexture = nullptr;
    }
    if (roughnessTexture && metalnessTexture && uvSetOf(roughnessTexture, mesh) != uvSetOf(metalnessTexture, mesh)) {
        warn("Material '" + name + "': its metalness map uses another UV set than the roughness and is not imported");
        metalnessTexture = nullptr;
    }

    const std::vector<int> texCoords = {
        texCoordFor(baseTexture ? baseTexture : opacityTexture, mesh),
        texCoordFor(roughnessTexture ? roughnessTexture : metalnessTexture, mesh),
        texCoordFor(normalTexture, mesh),
        texCoordFor(occlusionTexture, mesh),
        texCoordFor(emissionTexture, mesh)};

    tinygltf::Material out;
    out.name = name.empty() ? "Material" + std::to_string(material->typed_id) : name;
    tinygltf::PbrMetallicRoughness& pbrOut = out.pbrMetallicRoughness;

    // Color and texture together are unreliable in FBX, so a texture drops the color
    const float baseFactor = pbr.base_factor.has_value ? static_cast<float>(pbr.base_factor.value_real) : 1.0f;
    float color[4] = {baseFactor, baseFactor, baseFactor, 1.0f};
    if (!baseTexture && pbr.base_color.has_value) {
        color[0] = static_cast<float>(pbr.base_color.value_vec4.x) * baseFactor;
        color[1] = static_cast<float>(pbr.base_color.value_vec4.y) * baseFactor;
        color[2] = static_cast<float>(pbr.base_color.value_vec4.z) * baseFactor;
    }

    if (baseTexture || opacityTexture) {
        std::shared_ptr<Pixels> pixels;
        int image = opacityTexture ? combineOpacity(baseTexture, opacityTexture, pixels) : -1;
        const bool masked = image >= 0;
        if (!masked && baseTexture) {
            image = imageForFile(baseTexture->file_index);
            pixels = loadPixels(baseTexture->file_index);
        }
        const int texture = textureFor(image, baseTexture ? baseTexture : opacityTexture);
        if (texture >= 0) {
            pbrOut.baseColorTexture.index = texture;
            pbrOut.baseColorTexture.texCoord = texCoords[0];
            if (pixels) {
                out.alphaMode = detectAlphaMode(*pixels, !masked);
            }
        }
    }

    // ufbx sets opacity only from real opacity properties, so 0 means invisible
    const float opacity = pbr.opacity.has_value ? static_cast<float>(pbr.opacity.value_real) : 1.0f;
    if (opacity < 0.999f) {
        color[3] = opacity;
        out.alphaMode = "BLEND";
    }
    for (float& channel : color) {
        channel = std::clamp(channel, 0.0f, 1.0f);
    }
    pbrOut.baseColorFactor = {color[0], color[1], color[2], color[3]};

    float roughness = 1.0f;
    if (glossiness) {
        if (pbr.glossiness.has_value) {
            roughness = 1.0f - static_cast<float>(pbr.glossiness.value_real);
        }
    } else if (pbr.roughness.has_value) {
        roughness = static_cast<float>(pbr.roughness.value_real);
    }
    float metallic = pbr.metalness.has_value ? static_cast<float>(pbr.metalness.value_real) : 0.0f;

    if (roughnessTexture || metalnessTexture) {
        const int image = packMetallicRoughness(roughnessTexture, metalnessTexture, glossiness);
        const int texture = textureFor(image, roughnessTexture ? roughnessTexture : metalnessTexture);
        if (texture >= 0) {
            pbrOut.metallicRoughnessTexture.index = texture;
            pbrOut.metallicRoughnessTexture.texCoord = texCoords[1];
            if (roughnessTexture) roughness = 1.0f;
            if (metalnessTexture) metallic = 1.0f;
        }
    }
    pbrOut.metallicFactor = std::clamp(metallic, 0.0f, 1.0f);
    pbrOut.roughnessFactor = std::clamp(roughness, 0.0f, 1.0f);

    if (normalTexture) {
        const int texture = textureFor(imageForFile(normalTexture->file_index), normalTexture);
        if (texture >= 0) {
            out.normalTexture.index = texture;
            out.normalTexture.texCoord = texCoords[2];
        }
    }
    if (occlusionTexture) {
        const int texture = textureFor(imageForFile(occlusionTexture->file_index), occlusionTexture);
        if (texture >= 0) {
            out.occlusionTexture.index = texture;
            out.occlusionTexture.texCoord = texCoords[3];
        }
    }

    const float emissionFactor = pbr.emission_factor.has_value ? static_cast<float>(pbr.emission_factor.value_real) : 1.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    if (emissionTexture) {
        const int texture = textureFor(imageForFile(emissionTexture->file_index), emissionTexture);
        if (texture >= 0) {
            out.emissiveTexture.index = texture;
            out.emissiveTexture.texCoord = texCoords[4];
            emissive[0] = emissive[1] = emissive[2] = emissionFactor;
        }
    } else if (pbr.emission_color.has_value) {
        emissive[0] = static_cast<float>(pbr.emission_color.value_vec4.x) * emissionFactor;
        emissive[1] = static_cast<float>(pbr.emission_color.value_vec4.y) * emissionFactor;
        emissive[2] = static_cast<float>(pbr.emission_color.value_vec4.z) * emissionFactor;
    }
    out.emissiveFactor = {std::clamp(emissive[0], 0.0f, 1.0f), std::clamp(emissive[1], 0.0f, 1.0f),
        std::clamp(emissive[2], 0.0f, 1.0f)};
    out.doubleSided = material->features.double_sided.enabled;

    model.materials.push_back(out);
    const int index = static_cast<int>(model.materials.size()) - 1;
    materialIndices[key] = index;
    return index;
}

void GlbBuilder::buildGeometry(const ufbx_mesh* mesh) {
    const ufbx_vertex_vec2* uv0 = mesh->vertex_uv.exists ? &mesh->vertex_uv : nullptr;
    const ufbx_vertex_vec2* uv1 = mesh->uv_sets.count >= 2 && mesh->uv_sets.data[1].vertex_uv.exists ?
        &mesh->uv_sets.data[1].vertex_uv : nullptr;
    const bool hasColor = mesh->vertex_color.exists;
    std::string meshName = toString(mesh->name);
    if (meshName.empty()) {
        meshName = mesh->instances.count > 0 ? toString(mesh->instances.data[0]->name) : "Mesh" + std::to_string(mesh->typed_id);
    }

    // Blend shapes, one glTF morph target each (the full-weight shape)
    std::vector<const ufbx_blend_channel*> channels;
    std::vector<const ufbx_blend_shape*> shapes;
    for (const ufbx_blend_deformer* deformer : mesh->blend_deformers) {
        for (const ufbx_blend_channel* channel : deformer->channels) {
            const ufbx_blend_shape* shape = channel->target_shape;
            if (!shape && channel->keyframes.count > 0) {
                shape = channel->keyframes.data[channel->keyframes.count - 1].shape;
            }
            if (channel->keyframes.count > 1) {
                warn("Blend shape '" + toString(channel->name) + "' keeps only its full-weight shape, not the in-betweens");
            }
            if (shape) {
                channels.push_back(channel);
                shapes.push_back(shape);
            }
        }
    }
    if (shapes.size() > MAX_MORPHTARGETS) {
        warn("Mesh '" + meshName + "' has " + std::to_string(shapes.size()) + " blend shapes, only the first " +
            std::to_string(MAX_MORPHTARGETS) + " are rendered");
    }

    // Skin: glTF joints are nodes, deduplicated across clusters
    const ufbx_skin_deformer* skin = nullptr;
    for (const ufbx_skin_deformer* deformer : mesh->skin_deformers) {
        if (deformer->clusters.count > 0 && deformer->weights.count > 0) {
            skin = deformer;
            break;
        }
    }
    std::vector<int> jointOfCluster;
    std::vector<uint32_t> jointNodes;
    std::vector<float> inverseBinds;
    int fallbackJoint = -1;
    if (skin) {
        std::map<uint32_t, int> jointOfNode;
        auto addJoint = [&](const ufbx_node* node, const ufbx_matrix& inverseBind) {
            auto it = jointOfNode.find(node->typed_id);
            if (it != jointOfNode.end()) {
                return it->second;
            }
            const int joint = static_cast<int>(jointNodes.size());
            jointOfNode[node->typed_id] = joint;
            jointNodes.push_back(node->typed_id);
            appendMatrix(inverseBinds, inverseBind);
            return joint;
        };
        uint32_t fallbackDepth = UINT32_MAX;
        for (const ufbx_skin_cluster* cluster : skin->clusters) {
            jointOfCluster.push_back(cluster->bone_node ? addJoint(cluster->bone_node, cluster->geometry_to_bone) : -1);
            // Unweighted vertices follow the top bone, keeping a single skeleton root
            if (cluster->bone_node && cluster->bone_node->node_depth < fallbackDepth) {
                fallbackDepth = cluster->bone_node->node_depth;
                fallbackJoint = jointOfCluster.back();
            }
        }

        // The loader roots the skeleton at one joint and drops the transforms above it,
        // so ancestors such as an Armature join with no weight
        const ufbx_matrix meshToWorld = mesh->instances.count > 0 ?
            mesh->instances.data[0]->geometry_to_world : ufbx_identity_matrix;
        auto addAncestor = [&](const ufbx_node* node) {
            const ufbx_matrix worldToNode = ufbx_matrix_invert(&node->node_to_world);
            addJoint(node, ufbx_matrix_mul(&worldToNode, &meshToWorld));
        };
        std::set<const ufbx_node*> topNodes;
        const size_t clusterJoints = jointNodes.size();
        for (size_t j = 0; j < clusterJoints; j++) {
            const ufbx_node* top = scene->nodes.data[jointNodes[j]];
            for (const ufbx_node* node = top->parent; node && !node->is_root; node = node->parent) {
                addAncestor(node);
                top = node;
            }
            topNodes.insert(top);
        }
        if (topNodes.size() > 1) {
            addAncestor(scene->root_node);
        }
    }

    MeshGeometry geometry;
    geometry.name = meshName;
    std::vector<uint32_t> triangle(mesh->max_face_triangles * 3);
    for (const ufbx_mesh_part& part : mesh->material_parts) {
        if (part.num_triangles == 0) {
            continue;
        }

        std::vector<Vertex> vertices;
        vertices.reserve(part.num_triangles * 3);
        for (uint32_t faceIndex : part.face_indices) {
            const ufbx_face face = mesh->faces.data[faceIndex];
            if (face.num_indices < 3) {
                continue;
            }
            const uint32_t count = ufbx_triangulate_face(triangle.data(), triangle.size(), mesh, face);
            for (uint32_t i = 0; i < count * 3; i++) {
                const uint32_t index = triangle[i];
                Vertex vertex;
                std::memset(&vertex, 0, sizeof(vertex));

                const ufbx_vec3 position = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                vertex.position[0] = static_cast<float>(position.x);
                vertex.position[1] = static_cast<float>(position.y);
                vertex.position[2] = static_cast<float>(position.z);

                vertex.normal[1] = 1.0f;
                if (mesh->vertex_normal.exists) {
                    const ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
                    const double length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                    if (length > 1e-12) {
                        vertex.normal[0] = static_cast<float>(normal.x / length);
                        vertex.normal[1] = static_cast<float>(normal.y / length);
                        vertex.normal[2] = static_cast<float>(normal.z / length);
                    }
                }

                // FBX puts the UV origin at the bottom, glTF at the top
                if (uv0) {
                    const ufbx_vec2 uv = ufbx_get_vertex_vec2(uv0, index);
                    vertex.uv0[0] = static_cast<float>(uv.x);
                    vertex.uv0[1] = static_cast<float>(1.0 - uv.y);
                }
                if (uv1) {
                    const ufbx_vec2 uv = ufbx_get_vertex_vec2(uv1, index);
                    vertex.uv1[0] = static_cast<float>(uv.x);
                    vertex.uv1[1] = static_cast<float>(1.0 - uv.y);
                }
                if (hasColor) {
                    const ufbx_vec4 color = ufbx_get_vertex_vec4(&mesh->vertex_color, index);
                    vertex.color[0] = static_cast<float>(color.x);
                    vertex.color[1] = static_cast<float>(color.y);
                    vertex.color[2] = static_cast<float>(color.z);
                    vertex.color[3] = static_cast<float>(color.w);
                }
                // Keeps corners of different FBX vertices apart for skinning and morphs
                vertex.vertex = mesh->vertex_indices.data[index];
                vertices.push_back(vertex);
            }
        }
        if (vertices.empty()) {
            continue;
        }

        std::vector<uint32_t> indices(vertices.size());
        ufbx_vertex_stream stream = {vertices.data(), vertices.size(), sizeof(Vertex)};
        ufbx_error error;
        const size_t vertexCount = ufbx_generate_indices(&stream, 1, indices.data(), indices.size(), nullptr, &error);
        if (vertexCount == 0) {
            warn("Could not index mesh '" + meshName + "'");
            continue;
        }
        vertices.resize(vertexCount);

        tinygltf::Primitive primitive;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        std::vector<float> data;
        data.reserve(vertexCount * 4);

        for (const Vertex& vertex : vertices) data.insert(data.end(), vertex.position, vertex.position + 3);
        primitive.attributes["POSITION"] = addFloatAccessor(data, TINYGLTF_TYPE_VEC3, TINYGLTF_TARGET_ARRAY_BUFFER, true);

        data.clear();
        for (const Vertex& vertex : vertices) data.insert(data.end(), vertex.normal, vertex.normal + 3);
        primitive.attributes["NORMAL"] = addFloatAccessor(data, TINYGLTF_TYPE_VEC3, TINYGLTF_TARGET_ARRAY_BUFFER, false);

        if (uv0) {
            data.clear();
            for (const Vertex& vertex : vertices) data.insert(data.end(), vertex.uv0, vertex.uv0 + 2);
            primitive.attributes["TEXCOORD_0"] = addFloatAccessor(data, TINYGLTF_TYPE_VEC2, TINYGLTF_TARGET_ARRAY_BUFFER, false);
        }
        if (uv1) {
            data.clear();
            for (const Vertex& vertex : vertices) data.insert(data.end(), vertex.uv1, vertex.uv1 + 2);
            primitive.attributes["TEXCOORD_1"] = addFloatAccessor(data, TINYGLTF_TYPE_VEC2, TINYGLTF_TARGET_ARRAY_BUFFER, false);
        }
        if (hasColor) {
            data.clear();
            for (const Vertex& vertex : vertices) data.insert(data.end(), vertex.color, vertex.color + 4);
            primitive.attributes["COLOR_0"] = addFloatAccessor(data, TINYGLTF_TYPE_VEC4, TINYGLTF_TARGET_ARRAY_BUFFER, false);
        }

        if (skin && !jointNodes.empty()) {
            std::vector<uint16_t> joints(vertexCount * 4, 0);
            std::vector<float> weights(vertexCount * 4, 0.0f);
            for (size_t i = 0; i < vertexCount; i++) {
                fillWeights(skin, jointOfCluster, vertices[i].vertex, fallbackJoint, &joints[i * 4], &weights[i * 4]);
            }
            const int jointView = addBufferView(joints.data(), joints.size() * sizeof(uint16_t), TINYGLTF_TARGET_ARRAY_BUFFER);
            primitive.attributes["JOINTS_0"] = addAccessor(jointView, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, vertexCount, TINYGLTF_TYPE_VEC4);
            primitive.attributes["WEIGHTS_0"] = addFloatAccessor(weights, TINYGLTF_TYPE_VEC4, TINYGLTF_TARGET_ARRAY_BUFFER, false);
        }

        for (const ufbx_blend_shape* shape : shapes) {
            data.clear();
            for (const Vertex& vertex : vertices) {
                const ufbx_vec3 offset = ufbx_get_blend_shape_vertex_offset(shape, vertex.vertex);
                data.push_back(static_cast<float>(offset.x));
                data.push_back(static_cast<float>(offset.y));
                data.push_back(static_cast<float>(offset.z));
            }
            primitive.targets.push_back({{"POSITION", addFloatAccessor(data, TINYGLTF_TYPE_VEC3, TINYGLTF_TARGET_ARRAY_BUFFER, true)}});
        }

        if (vertexCount <= 65535) {
            std::vector<uint16_t> shortIndices(indices.begin(), indices.end());
            const int view = addBufferView(shortIndices.data(), shortIndices.size() * sizeof(uint16_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            primitive.indices = addAccessor(view, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, shortIndices.size(), TINYGLTF_TYPE_SCALAR);
        } else {
            const int view = addBufferView(indices.data(), indices.size() * sizeof(uint32_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            primitive.indices = addAccessor(view, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, indices.size(), TINYGLTF_TYPE_SCALAR);
        }

        geometry.primitives.push_back(primitive);
        geometry.slots.push_back(part.index);
    }

    if (geometry.primitives.empty()) {
        return;
    }
    geometry.channels = channels;

    if (skin && !jointNodes.empty()) {
        tinygltf::Skin skinOut;
        skinOut.name = toString(skin->name).empty() ? meshName + "Skin" : toString(skin->name);
        skinOut.joints.assign(jointNodes.begin(), jointNodes.end());
        skinOut.inverseBindMatrices = addFloatAccessor(inverseBinds, TINYGLTF_TYPE_MAT4, 0, false);
        model.skins.push_back(skinOut);
        geometry.skin = static_cast<int>(model.skins.size()) - 1;
    }
    geometries[mesh->typed_id] = std::move(geometry);
}

// FBX binds materials per node, so each material set gets its own mesh over shared accessors
int GlbBuilder::meshFor(const ufbx_node* node) {
    auto geometry = geometries.find(node->mesh->typed_id);
    if (geometry == geometries.end()) {
        return -1;
    }
    std::vector<const ufbx_material*> slots(node->materials.begin(), node->materials.end());
    if (slots.empty()) {
        slots.assign(node->mesh->materials.begin(), node->mesh->materials.end());
    }
    const auto key = std::make_pair(node->mesh->typed_id, slots);
    auto cached = meshIndices.find(key);
    if (cached != meshIndices.end()) {
        return cached->second;
    }

    const MeshGeometry& data = geometry->second;
    tinygltf::Mesh out;
    out.name = data.name;
    out.primitives = data.primitives;
    for (size_t i = 0; i < out.primitives.size(); i++) {
        const uint32_t slot = data.slots[i];
        if (slot < slots.size() && slots[slot]) {
            out.primitives[i].material = materialFor(slots[slot], node->mesh);
        }
    }
    if (!data.channels.empty()) {
        tinygltf::Value::Array names;
        for (size_t i = 0; i < data.channels.size(); i++) {
            out.weights.push_back(data.channels[i]->weight);
            const std::string name = toString(data.channels[i]->name);
            names.emplace_back(name.empty() ? "Morph" + std::to_string(i) : name);
        }
        out.extras = tinygltf::Value(tinygltf::Value::Object{{"targetNames", tinygltf::Value(names)}});
    }
    model.meshes.push_back(out);
    const int index = static_cast<int>(model.meshes.size()) - 1;
    meshIndices[key] = index;
    return index;
}

void GlbBuilder::buildNodes() {
    std::set<std::string> usedNames;
    for (const ufbx_node* node : scene->nodes) {
        tinygltf::Node out;

        // Unique names: re-imports match moved parts by name
        std::string name = toString(node->name);
        if (name.empty()) {
            name = node->is_root ? "RootNode" : "Node" + std::to_string(node->typed_id);
        }
        std::string unique = name;
        for (int suffix = 2; usedNames.count(unique); suffix++) {
            unique = name + "_" + std::to_string(suffix);
        }
        usedNames.insert(unique);
        out.name = unique;

        const ufbx_transform& transform = node->local_transform;
        const ufbx_vec3& t = transform.translation;
        const ufbx_quat& r = transform.rotation;
        const ufbx_vec3& s = transform.scale;
        if (t.x != 0.0 || t.y != 0.0 || t.z != 0.0) {
            out.translation = {t.x, t.y, t.z};
        }
        if (r.x != 0.0 || r.y != 0.0 || r.z != 0.0 || r.w != 1.0) {
            out.rotation = {r.x, r.y, r.z, r.w};
        }
        if (s.x != 1.0 || s.y != 1.0 || s.z != 1.0) {
            out.scale = {s.x, s.y, s.z};
        }
        for (const ufbx_node* child : node->children) {
            out.children.push_back(static_cast<int>(child->typed_id));
        }

        if (node->mesh) {
            out.mesh = meshFor(node);
            if (out.mesh >= 0) {
                out.skin = geometries.at(node->mesh->typed_id).skin;
            }
        }
        model.nodes.push_back(out);
    }
}

void GlbBuilder::buildAnimations() {
    std::set<std::string> usedNames;
    for (const ufbx_anim_stack* stack : scene->anim_stacks) {
        ufbx_bake_opts options = {};
        options.trim_start_time = true;
        options.resample_rate = 30.0;
        options.minimum_sample_rate = 30.0;
        options.max_keyframe_segments = 1024;
        options.key_reduction_enabled = true;
        options.key_reduction_rotation = true;

        ufbx_error error;
        ufbx_baked_anim* baked = ufbx_bake_anim(scene, stack->anim, &options, &error);
        if (!baked) {
            char message[512];
            ufbx_format_error(message, sizeof(message), &error);
            warn("Animation '" + toString(stack->name) + "' was skipped: " + message);
            continue;
        }
        std::unique_ptr<ufbx_baked_anim, decltype(&ufbx_free_baked_anim)> bakedGuard(baked, &ufbx_free_baked_anim);

        tinygltf::Animation animation;
        auto addChannel = [&](uint32_t node, const char* path, const std::vector<float>& times,
                              const std::vector<float>& values, int type) {
            tinygltf::AnimationSampler sampler;
            sampler.input = addFloatAccessor(times, TINYGLTF_TYPE_SCALAR, 0, true);
            sampler.output = addFloatAccessor(values, type, 0, false);
            animation.samplers.push_back(sampler);

            tinygltf::AnimationChannel channel;
            channel.sampler = static_cast<int>(animation.samplers.size()) - 1;
            channel.target_node = static_cast<int>(node);
            channel.target_path = path;
            animation.channels.push_back(channel);
        };

        std::vector<float> times;
        std::vector<float> values;
        for (const ufbx_baked_node& track : baked->nodes) {
            if (track.translation_keys.count > 0) {
                times.clear();
                values.clear();
                for (const ufbx_baked_vec3& key : track.translation_keys) {
                    times.push_back(static_cast<float>(key.time));
                    values.insert(values.end(), {static_cast<float>(key.value.x), static_cast<float>(key.value.y),
                        static_cast<float>(key.value.z)});
                }
                addChannel(track.typed_id, "translation", times, values, TINYGLTF_TYPE_VEC3);
            }
            if (track.rotation_keys.count > 0) {
                times.clear();
                values.clear();
                for (const ufbx_baked_quat& key : track.rotation_keys) {
                    times.push_back(static_cast<float>(key.time));
                    values.insert(values.end(), {static_cast<float>(key.value.x), static_cast<float>(key.value.y),
                        static_cast<float>(key.value.z), static_cast<float>(key.value.w)});
                }
                addChannel(track.typed_id, "rotation", times, values, TINYGLTF_TYPE_VEC4);
            }
            if (track.scale_keys.count > 0) {
                times.clear();
                values.clear();
                for (const ufbx_baked_vec3& key : track.scale_keys) {
                    times.push_back(static_cast<float>(key.time));
                    values.insert(values.end(), {static_cast<float>(key.value.x), static_cast<float>(key.value.y),
                        static_cast<float>(key.value.z)});
                }
                addChannel(track.typed_id, "scale", times, values, TINYGLTF_TYPE_VEC3);
            }
        }

        // glTF animates all morph weights of a node together, on shared key times
        std::map<uint32_t, const ufbx_baked_prop*> weightTracks;
        for (const ufbx_baked_element& element : baked->elements) {
            const ufbx_blend_channel* channel = ufbx_as_blend_channel(scene->elements.data[element.element_id]);
            if (!channel) {
                continue;
            }
            for (const ufbx_baked_prop& prop : element.props) {
                if (toString(prop.name) == UFBX_DeformPercent) {
                    weightTracks[channel->typed_id] = &prop;
                }
            }
        }
        if (!weightTracks.empty()) {
            for (const ufbx_node* node : scene->nodes) {
                if (!node->mesh) {
                    continue;
                }
                auto geometry = geometries.find(node->mesh->typed_id);
                if (geometry == geometries.end() || geometry->second.channels.empty()) {
                    continue;
                }
                const std::vector<const ufbx_blend_channel*>& meshChannels = geometry->second.channels;
                std::set<double> keyTimes;
                for (const ufbx_blend_channel* channel : meshChannels) {
                    auto track = weightTracks.find(channel->typed_id);
                    if (track != weightTracks.end()) {
                        for (const ufbx_baked_vec3& key : track->second->keys) {
                            keyTimes.insert(key.time);
                        }
                    }
                }
                if (keyTimes.empty()) {
                    continue;
                }
                times.clear();
                values.clear();
                for (double time : keyTimes) {
                    times.push_back(static_cast<float>(time));
                    for (const ufbx_blend_channel* channel : meshChannels) {
                        auto track = weightTracks.find(channel->typed_id);
                        values.push_back(sampleWeight(track != weightTracks.end() ? track->second : nullptr, time,
                            static_cast<float>(channel->weight)));
                    }
                }
                addChannel(node->typed_id, "weights", times, values, TINYGLTF_TYPE_SCALAR);
            }
        }

        if (animation.channels.empty()) {
            continue;
        }
        std::string name = toString(stack->name);
        if (name.empty()) {
            name = "Animation" + std::to_string(model.animations.size());
        }
        std::string unique = name;
        for (int suffix = 2; usedNames.count(unique); suffix++) {
            unique = name + "_" + std::to_string(suffix);
        }
        usedNames.insert(unique);
        animation.name = unique;
        model.animations.push_back(animation);
    }
}

std::shared_ptr<GlbBuilder::Pixels> GlbBuilder::resized(const std::shared_ptr<Pixels>& pixels, int width, int height) {
    if (!pixels || (pixels->width == width && pixels->height == height)) {
        return pixels;
    }
    auto out = std::make_shared<Pixels>();
    out->width = width;
    out->height = height;
    out->rgba.resize(static_cast<size_t>(width) * height * 4);
    // Channels are independent data, not color with alpha
    if (!stbir_resize_uint8_linear(pixels->rgba.data(), pixels->width, pixels->height, 0,
            out->rgba.data(), width, height, 0, STBIR_4CHANNEL)) {
        return nullptr;
    }
    return out;
}

// Mostly-binary alpha with soft edges still reads as a cutout
const char* GlbBuilder::detectAlphaMode(const Pixels& pixels, bool emptyAlphaIsJunk) {
    size_t transparent = 0;
    size_t partial = 0;
    const size_t count = pixels.rgba.size() / 4;
    for (size_t i = 0; i < count; i++) {
        const unsigned char alpha = pixels.rgba[i * 4 + 3];
        if (alpha <= 5) {
            transparent++;
        } else if (alpha < 250) {
            partial++;
        }
    }
    if (transparent == 0 && partial == 0) {
        return "OPAQUE";
    }
    // An all-zero alpha channel is junk, unless it came from an opacity map
    if (transparent == count && emptyAlphaIsJunk) {
        return "OPAQUE";
    }
    return partial * 10 <= transparent + partial ? "MASK" : "BLEND";
}

}
