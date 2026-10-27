// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatAssetWriter.h"

#include "util/FileUtils.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using namespace jasperengine;
using jasperengine::editor::FileUtils;

namespace{

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    bool endsWith(const std::string& text, const std::string& suffix){
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // a relative path as a glTF URI (percent-encoded, '/' kept)
    std::string encodeUri(const std::string& path){
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : path){
            const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                    c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
            if (unreserved){
                out += (char)c;
            }else{
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }
        return out;
    }

    // per-vertex attribute arrays: empty or `width` floats per vertex
    bool checkAttribute(size_t size, size_t vertices, size_t width, const char* name, bool required, std::string* error){
        if (size == 0 && !required) return true;
        if (size != vertices * width){
            return fail(error, std::string("mesh ") + name + " has " + std::to_string(size) + " values, " +
                        std::to_string(vertices) + " vertices need " + std::to_string(vertices * width));
        }
        return true;
    }

    class GlbWriter{
    public:
        tinygltf::Model model;

        GlbWriter(){
            model.buffers.emplace_back();
        }

        int addView(const void* data, size_t size, int target){
            std::vector<unsigned char>& bin = model.buffers[0].data;
            while (bin.size() % 4 != 0) bin.push_back(0);
            tinygltf::BufferView view;
            view.buffer = 0;
            view.byteOffset = bin.size();
            view.byteLength = size;
            view.target = target;
            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            bin.insert(bin.end(), bytes, bytes + size);
            model.bufferViews.push_back(view);
            return (int)model.bufferViews.size() - 1;
        }

        int addAccessor(int view, int componentType, size_t count, int type){
            tinygltf::Accessor accessor;
            accessor.bufferView = view;
            accessor.componentType = componentType;
            accessor.count = count;
            accessor.type = type;
            model.accessors.push_back(accessor);
            return (int)model.accessors.size() - 1;
        }

        int addFloats(const std::vector<float>& values, int type, bool bounds){
            const int components = tinygltf::GetNumComponentsInType(type);
            const size_t count = values.size() / (size_t)components;
            int view = addView(values.data(), values.size() * sizeof(float), TINYGLTF_TARGET_ARRAY_BUFFER);
            int index = addAccessor(view, TINYGLTF_COMPONENT_TYPE_FLOAT, count, type);
            if (bounds && count > 0){
                tinygltf::Accessor& accessor = model.accessors[index];
                accessor.minValues.assign(components, DBL_MAX);
                accessor.maxValues.assign(components, -DBL_MAX);
                for (size_t i = 0; i < count; i++){
                    for (int c = 0; c < components; c++){
                        double v = values[i * components + c];
                        accessor.minValues[c] = std::min(accessor.minValues[c], v);
                        accessor.maxValues[c] = std::max(accessor.maxValues[c], v);
                    }
                }
            }
            return index;
        }

        // one image + texture per URI
        int textureFor(const std::string& uri){
            auto cached = textures.find(uri);
            if (cached != textures.end()) return cached->second;
            tinygltf::Image image;
            image.uri = uri;
            model.images.push_back(image);
            tinygltf::Texture texture;
            texture.source = (int)model.images.size() - 1;
            model.textures.push_back(texture);
            int index = (int)model.textures.size() - 1;
            textures[uri] = index;
            return index;
        }

        std::vector<uint8_t> write(){
            tinygltf::TinyGLTF writer;
            // the default image writer cuts a URI down to its file name: keep it as given
            writer.SetImageWriter([](const std::string*, const std::string*, const tinygltf::Image* image, bool,
                                     const tinygltf::FsCallbacks*, const tinygltf::URICallbacks*, std::string* uri, void*){
                *uri = image->uri;
                return true;
            }, nullptr);
            std::ostringstream stream;
            if (!writer.WriteGltfSceneToStream(&model, stream, false, true)) return {};
            const std::string bytes = stream.str();
            return std::vector<uint8_t>(bytes.begin(), bytes.end());
        }

    private:
        std::map<std::string, int> textures;
    };

    // the URI of an absolute texture file relative to the glb's directory; false when there is none
    bool textureUri(const std::string& file, const fs::path& glbDir, std::string& uri, std::string* error){
        std::error_code ec;
        fs::path absolute = fs::absolute(FileUtils::pathFromUtf8(file), ec).lexically_normal();
        if (ec) return fail(error, "texture " + file + ": " + ec.message());
        fs::path relative = absolute.lexically_relative(glbDir);
        if (relative.empty()){
            return fail(error, "texture " + file + " can't be referenced from " + FileUtils::pathToGenericUtf8(glbDir) +
                        " (another drive or root)");
        }
        uri = encodeUri(FileUtils::pathToGenericUtf8(relative));
        return true;
    }

    bool buildMaterial(GlbWriter& glb, const editor::vat::VatMaterialData& source, const fs::path& glbDir,
                       tinygltf::Material& material, std::string* error){
        material.name = source.name;
        material.pbrMetallicRoughness.baseColorFactor.assign(source.baseColor, source.baseColor + 4);
        material.pbrMetallicRoughness.metallicFactor = source.metallic;
        material.pbrMetallicRoughness.roughnessFactor = source.roughness;
        material.emissiveFactor.assign(source.emissive, source.emissive + 3);
        if (source.alphaMode != "OPAQUE" && source.alphaMode != "MASK" && source.alphaMode != "BLEND"){
            return fail(error, "material \"" + source.name + "\": alphaMode \"" + source.alphaMode + "\", expected OPAQUE, MASK or BLEND");
        }
        material.alphaMode = source.alphaMode;
        material.alphaCutoff = source.alphaCutoff;
        material.doubleSided = source.doubleSided;
        struct Slot{
            const std::string* file;
            int* index;
        };
        const Slot slots[] = {
            {&source.baseColorTexture, &material.pbrMetallicRoughness.baseColorTexture.index},
            {&source.metallicRoughnessTexture, &material.pbrMetallicRoughness.metallicRoughnessTexture.index},
            {&source.normalTexture, &material.normalTexture.index},
            {&source.occlusionTexture, &material.occlusionTexture.index},
            {&source.emissiveTexture, &material.emissiveTexture.index},
        };
        for (const Slot& slot : slots){
            if (slot.file->empty()) continue;
            std::string uri;
            if (!textureUri(*slot.file, glbDir, uri, error)) return false;
            *slot.index = glb.textureFor(uri);
        }
        return true;
    }

}

namespace jasperengine::editor::vat{

std::string meshPathFor(const std::string& vatPath){
    const std::string extension = jasperengine::vat::VAT_EXTENSION;
    if (endsWith(vatPath, extension)) return vatPath + ".glb";
    fs::path path = FileUtils::pathFromUtf8(vatPath);
    return FileUtils::pathToGenericUtf8(path.parent_path() / FileUtils::pathFromUtf8(FileUtils::pathToUtf8(path.stem()) +
                                                                                    jasperengine::vat::VAT_MESH_SUFFIX));
}

bool encodeVatFiles(const std::string& vatPath, jasperengine::vat::VatData& data, const VatMeshData& mesh,
                                 const jasperengine::vat::VatPayload& payload, const jasperengine::vat::VatSocketTracks& sockets,
                                 VatEncodedFiles& out, std::string* error){
    using namespace jasperengine::vat;
    out = VatEncodedFiles();

    const std::string vatExtension = VAT_EXTENSION;
    fs::path vatFile = FileUtils::pathFromUtf8(vatPath);
    if (!endsWith(vatPath, vatExtension) || FileUtils::pathToUtf8(vatFile.filename()) == vatExtension){
        return fail(error, "the output must be a .vat file: " + vatPath);
    }
    const std::string meshPath = meshPathFor(vatPath);

    // payload vs layout
    const VatLayout& layout = data.layout;
    if (data.mode == VatMode::Bones && layout.precision != VatPrecision::Float){
        return fail(error, "a bones payload is always float precision");
    }
    if (payload.precision != layout.precision || payload.width != layout.width || payload.height != layout.height){
        return fail(error, "the payload is " + std::to_string(payload.width) + " x " + std::to_string(payload.height) + " " +
                    precisionName(payload.precision) + ", the layout says " + std::to_string(layout.width) + " x " +
                    std::to_string(layout.height) + " " + precisionName(layout.precision));
    }
    if (payload.width < 1 || payload.height < 1){
        return fail(error, "the payload is empty (" + std::to_string(payload.width) + " x " + std::to_string(payload.height) + ")");
    }
    const uint64_t texelBytes = payload.precision == VatPrecision::Float ? 16 : 8;
    const uint64_t payloadBytes = (uint64_t)payload.width * (uint64_t)payload.height * texelBytes;
    if ((uint64_t)payload.bytes.size() != payloadBytes){
        return fail(error, "the payload has " + std::to_string(payload.bytes.size()) + " bytes, " + std::to_string(payload.width) +
                    " x " + std::to_string(payload.height) + " " + precisionName(payload.precision) + " texels are " +
                    std::to_string(payloadBytes));
    }
    if (data.clips.empty()) return fail(error, "no clips: nothing to play");
    if (data.clips.size() > (size_t)VAT_MAX_CLIPS){
        return fail(error, std::to_string(data.clips.size()) + " clips, at most " + std::to_string(VAT_MAX_CLIPS));
    }

    // the mesh
    const size_t vertices = mesh.vertexCount();
    if (vertices == 0 || mesh.positions.size() % 3 != 0){
        return fail(error, "mesh positions: " + std::to_string(mesh.positions.size()) + " values, expected 3 per vertex (at least one)");
    }
    if (vertices > (size_t)VAT_MAX_VERTICES){
        return fail(error, "mesh has " + std::to_string(vertices) + " vertices, at most " + std::to_string(VAT_MAX_VERTICES));
    }
    if (!checkAttribute(mesh.normals.size(), vertices, 3, "normals", false, error) ||
        !checkAttribute(mesh.tangents.size(), vertices, 4, "tangents", false, error) ||
        !checkAttribute(mesh.uv0.size(), vertices, 2, "uv0", false, error) ||
        !checkAttribute(mesh.uv1.size(), vertices, 2, "uv1", false, error) ||
        !checkAttribute(mesh.colors.size(), vertices, 4, "colors", false, error) ||
        !checkAttribute(mesh.vatIndex.size(), vertices, 1, "vatIndex", true, error)){
        return false;
    }
    if (data.mode == VatMode::Vertices){
        if (!mesh.joints.empty() || !mesh.weights.empty()){
            return fail(error, "joints/weights are for bones mode: a vertices bake has no skin");
        }
    }else{
        if (!checkAttribute(mesh.joints.size(), vertices, 4, "joints", true, error) ||
            !checkAttribute(mesh.weights.size(), vertices, 4, "weights", true, error)){
            return false;
        }
        for (size_t i = 0; i < mesh.joints.size(); i++){
            if ((int)mesh.joints[i] >= layout.bones){
                return fail(error, "mesh joint " + std::to_string(mesh.joints[i]) + " (vertex " + std::to_string(i / 4) +
                            ") is past the " + std::to_string(layout.bones) + " baked bones");
            }
        }
    }
    for (size_t i = 0; i < vertices; i++){
        float index = mesh.vatIndex[i];
        if (!(index >= 0.0f) || index != std::floor(index) || index >= (float)VAT_MAX_VERTICES ||
            (data.mode == VatMode::Vertices && index >= (float)layout.vertices)){
            return fail(error, "vatIndex of vertex " + std::to_string(i) + " is " + std::to_string(index) +
                        ", expected a whole number below " +
                        std::to_string(data.mode == VatMode::Vertices ? layout.vertices : VAT_MAX_VERTICES));
        }
    }
    if (mesh.submeshes.empty()) return fail(error, "the mesh has no submeshes");
    for (size_t s = 0; s < mesh.submeshes.size(); s++){
        const VatSubmesh& submesh = mesh.submeshes[s];
        if (submesh.indices.empty() || submesh.indices.size() % 3 != 0){
            return fail(error, "submesh " + std::to_string(s) + " has " + std::to_string(submesh.indices.size()) +
                        " indices, expected whole triangles (at least one)");
        }
        for (uint32_t index : submesh.indices){
            if (index >= vertices){
                return fail(error, "submesh " + std::to_string(s) + " index " + std::to_string(index) + " is past the " +
                            std::to_string(vertices) + " vertices");
            }
        }
        if (submesh.material < -1 || submesh.material >= (int)mesh.materials.size()){
            return fail(error, "submesh " + std::to_string(s) + " material " + std::to_string(submesh.material) + " doesn't exist (" +
                        std::to_string(mesh.materials.size()) + " materials)");
        }
    }

    // socket tracks
    if (sockets.sockets < 0 || sockets.frames < 0 ||
        (uint64_t)sockets.data.size() != (uint64_t)sockets.sockets * (uint64_t)sockets.frames * 7){
        return fail(error, "the socket tracks have " + std::to_string(sockets.data.size()) + " floats, expected sockets x frames x 7");
    }
    if (sockets.sockets > 0 && sockets.frames != layout.samples){
        return fail(error, "the socket tracks have " + std::to_string(sockets.frames) + " frames, the layout " +
                    std::to_string(layout.samples) + " samples");
    }
    if (!data.recipe.sockets.empty() && (int)data.recipe.sockets.size() != sockets.sockets){
        return fail(error, "recipe.sockets lists " + std::to_string(data.recipe.sockets.size()) + " sockets, the tracks have " +
                    std::to_string(sockets.sockets));
    }
    if (data.recipe.sockets.empty() && data.sockets.size() > (size_t)sockets.sockets){
        return fail(error, std::to_string(data.sockets.size()) + " sockets, the tracks have " + std::to_string(sockets.sockets));
    }

    // the generated fields, then the JSON, which must read back clean and unchanged
    data.mesh = FileUtils::pathToUtf8(FileUtils::pathFromUtf8(meshPath).filename());
    data.layout.layoutHash = computeLayoutHash(data);
    data.stats.bytes = payload.bytes.size();
    std::string text = writeVat(data);
    VatParseResult parsed = parseVat(text);
    if (!parsed.ok()) return fail(error, "the .vat would not load: " + parsed.error);
    for (const ClipWarning& warning : parsed.warnings){
        // unknown keys are kept on purpose
        if (warning.path.empty()) continue;
        return fail(error, "the .vat would not load as written: " + warning.path + ": " + warning.message);
    }
    if (writeVat(*parsed.data) != text){
        return fail(error, "the .vat would not read back unchanged (non-canonical data)");
    }

    // the glb
    std::error_code ec;
    fs::path glbDir = fs::absolute(FileUtils::pathFromUtf8(meshPath), ec).lexically_normal().parent_path();
    if (ec) return fail(error, "cannot resolve " + meshPath + ": " + ec.message());

    GlbWriter glb;
    tinygltf::Model& model = glb.model;
    model.asset.version = "2.0";
    model.asset.generator = "JasperEngine vertex animation";

    for (const VatMaterialData& source : mesh.materials){
        tinygltf::Material material;
        if (!buildMaterial(glb, source, glbDir, material, error)) return false;
        model.materials.push_back(std::move(material));
    }

    std::map<std::string, int> attributes;
    attributes["POSITION"] = glb.addFloats(mesh.positions, TINYGLTF_TYPE_VEC3, true);
    if (!mesh.normals.empty()) attributes["NORMAL"] = glb.addFloats(mesh.normals, TINYGLTF_TYPE_VEC3, false);
    if (!mesh.tangents.empty()) attributes["TANGENT"] = glb.addFloats(mesh.tangents, TINYGLTF_TYPE_VEC4, false);
    if (!mesh.uv0.empty()) attributes["TEXCOORD_0"] = glb.addFloats(mesh.uv0, TINYGLTF_TYPE_VEC2, false);
    if (!mesh.uv1.empty()) attributes["TEXCOORD_1"] = glb.addFloats(mesh.uv1, TINYGLTF_TYPE_VEC2, false);
    if (!mesh.colors.empty()) attributes["COLOR_0"] = glb.addFloats(mesh.colors, TINYGLTF_TYPE_VEC4, false);
    if (!mesh.joints.empty()){
        int view = glb.addView(mesh.joints.data(), mesh.joints.size() * sizeof(uint16_t), TINYGLTF_TARGET_ARRAY_BUFFER);
        attributes["JOINTS_0"] = glb.addAccessor(view, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, vertices, TINYGLTF_TYPE_VEC4);
        attributes["WEIGHTS_0"] = glb.addFloats(mesh.weights, TINYGLTF_TYPE_VEC4, false);
    }
    attributes[VAT_INDEX_ATTRIBUTE] = glb.addFloats(mesh.vatIndex, TINYGLTF_TYPE_SCALAR, false);

    const std::string stem = FileUtils::pathToUtf8(vatFile.stem());
    tinygltf::Mesh gltfMesh;
    gltfMesh.name = stem;
    for (const VatSubmesh& submesh : mesh.submeshes){
        tinygltf::Primitive primitive;
        primitive.attributes = attributes;
        int view = glb.addView(submesh.indices.data(), submesh.indices.size() * sizeof(uint32_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        primitive.indices = glb.addAccessor(view, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, submesh.indices.size(), TINYGLTF_TYPE_SCALAR);
        primitive.material = submesh.material;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        gltfMesh.primitives.push_back(std::move(primitive));
    }
    model.meshes.push_back(std::move(gltfMesh));

    tinygltf::Node node;
    node.name = stem;
    node.mesh = 0;
    model.nodes.push_back(node);
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    // the payload and socket tracks: plain buffer views (no target, not vertex data)
    tinygltf::Value::Object payloadInfo;
    payloadInfo["bufferView"] = tinygltf::Value(glb.addView(payload.bytes.data(), payload.bytes.size(), 0));
    payloadInfo["format"] = tinygltf::Value(std::string(precisionName(payload.precision)));
    payloadInfo["width"] = tinygltf::Value(payload.width);
    payloadInfo["height"] = tinygltf::Value(payload.height);
    tinygltf::Value::Object extension;
    extension["version"] = tinygltf::Value(VAT_VERSION);
    extension["payload"] = tinygltf::Value(std::move(payloadInfo));
    extension["layoutHash"] = tinygltf::Value(data.layout.layoutHash);
    if (sockets.sockets > 0){
        tinygltf::Value::Object socketInfo;
        socketInfo["bufferView"] = tinygltf::Value(glb.addView(sockets.data.data(), sockets.data.size() * sizeof(float), 0));
        socketInfo["count"] = tinygltf::Value(sockets.sockets);
        socketInfo["frames"] = tinygltf::Value(sockets.frames);
        extension["sockets"] = tinygltf::Value(std::move(socketInfo));
    }
    model.extensions[VAT_GLTF_EXTENSION] = tinygltf::Value(std::move(extension));
    // used, not required: a viewer without it still shows the rest pose
    model.extensionsUsed.push_back(VAT_GLTF_EXTENSION);

    std::vector<uint8_t> bytes = glb.write();
    if (bytes.empty()) return fail(error, "cannot encode " + meshPath);

    out.vatPath = vatPath;
    out.meshPath = meshPath;
    out.vatText = std::move(text);
    out.glbBytes = std::move(bytes);
    return true;
}

}
