// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelSourceDecode.h"

#include "util/FileUtils.h"

#include "stb_image_write.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using Json = GltfDocument::Json;

    const char* const DRACO = "KHR_draco_mesh_compression";
    const char* const WEBP = "EXT_texture_webp";
    const char* const BASISU = "KHR_texture_basisu";

    // external images are never read by the inspection: it wants geometry and the load's findings
    bool existsUnlessImage(const std::string& path, void* userData){
        std::string ext = fs::path(path).extension().generic_string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        for (const char* image : {".png", ".jpg", ".jpeg", ".webp", ".ktx", ".ktx2", ".basis", ".dds", ".tga", ".bmp", ".gif", ".hdr", ".exr"}){
            if (ext == image)
                return false;
        }
        return tinygltf::FileExists(path, userData);
    }

    void removeFromList(Json& root, const char* key, const std::string& name){
        if (!root.contains(key) || !root[key].is_array())
            return;
        Json& list = root[key];
        for (auto it = list.begin(); it != list.end();){
            if (it->is_string() && it->get<std::string>() == name)
                it = list.erase(it);
            else
                ++it;
        }
        if (list.empty())
            root.erase(key);
    }

    bool anyPrimitiveUses(const Json& root, const char* extension){
        const Json* meshes = GltfDocument::arrayOf(root, "meshes");
        if (!meshes)
            return false;
        for (const Json& mesh : *meshes){
            if (!mesh.is_object() || !mesh.contains("primitives") || !mesh["primitives"].is_array())
                continue;
            for (const Json& primitive : mesh["primitives"]){
                if (primitive.is_object() && primitive.contains("extensions") && primitive["extensions"].is_object() &&
                        primitive["extensions"].contains(extension))
                    return true;
            }
        }
        return false;
    }

    void appendBytes(void* context, void* data, int size){
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<const uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    }

    // an image's pixels as an embedded PNG: a view of `doc`; false with a reason
    bool pngView(GltfDocument& doc, const std::vector<uint8_t>& encoded, const std::string& what, size_t& view, std::string& error){
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        std::string decodeError;
        if (!GltfNormalize::decodeImage(encoded.data(), encoded.size(), rgba, width, height, decodeError)){
            error = what + " does not decode" + (decodeError.empty() ? std::string() : ": " + decodeError);
            return false;
        }
        std::vector<uint8_t> png;
        if (width <= 0 || height <= 0 || !stbi_write_png_to_func(&appendBytes, &png, width, height, 4, rgba.data(), width * 4) || png.empty()){
            error = what + " does not encode as PNG";
            return false;
        }
        view = doc.addView(std::move(png));
        return true;
    }

}

bool ModelSourceDecode::inspect(const fs::path& file, const std::string& label, ModelSourceLoad& out, std::string& error){
    std::vector<uint8_t> bytes;
    {
        std::ifstream in(file, std::ios::binary);
        if (!in){
            error = "cannot read '" + file.generic_string() + "'";
            return false;
        }
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    out.model = std::make_shared<tinygltf::Model>();
    tinygltf::TinyGLTF loader;
    loader.SetFsCallbacks({&existsUnlessImage, &tinygltf::ExpandFilePath, &tinygltf::ReadWholeFile, &tinygltf::WriteWholeFile,
                           &tinygltf::GetFileSizeInBytes, nullptr});
    // embedded images stay encoded: the transcoding below reads them from the cook's document
    loader.SetImagesAsIs(true);
    std::string err, warn;
    const std::string baseDir = FileUtils::pathToUtf8(file.parent_path());
    const bool binary = bytes.size() >= 4 && bytes[0] == 'g' && bytes[1] == 'l' && bytes[2] == 'T' && bytes[3] == 'F';
    const bool parsed = binary ? loader.LoadBinaryFromMemory(out.model.get(), &err, &warn, bytes.data(), (unsigned int)bytes.size(), baseDir)
                               : loader.LoadASCIIFromString(out.model.get(), &err, &warn, reinterpret_cast<const char*>(bytes.data()),
                                                            (unsigned int)bytes.size(), baseDir);
    if (!parsed || !err.empty()){
        error = label + " does not parse: " + (err.empty() ? std::string("not a glTF file") : err);
        return false;
    }
    if (!GltfNormalize::normalize(*out.model, label, out.result)){
        error = out.result.firstError();
        if (error.empty())
            error = label + " cannot be loaded";
        return false;
    }
    return true;
}

bool ModelSourceDecode::takeDraco(GltfDocument& doc, const ModelSourceLoad& load, std::string& error){
    Json& root = doc.json();
    if (!anyPrimitiveUses(root, DRACO)){
        // listed but used by no primitive: nothing to decode, and nothing a player needs
        removeFromList(root, "extensionsRequired", DRACO);
        removeFromList(root, "extensionsUsed", DRACO);
        return true;
    }
    const tinygltf::Model& model = *load.model;
    doc.prepareViews();
    Json& meshes = root["meshes"];
    std::map<size_t, size_t> moved;     // accessor -> its new view
    for (size_t m = 0; m < meshes.size(); m++){
        if (!meshes[m].is_object() || !meshes[m].contains("primitives") || !meshes[m]["primitives"].is_array())
            continue;
        Json& list = meshes[m]["primitives"];
        for (size_t p = 0; p < list.size(); p++){
            Json& primitive = list[p];
            if (!primitive.is_object() || !primitive.contains("extensions") || !primitive["extensions"].is_object() ||
                    !primitive["extensions"].contains(DRACO))
                continue;
            const std::string where = "mesh " + std::to_string(m) + " primitive " + std::to_string(p);
            std::vector<size_t> accessors;
            if (primitive.contains("attributes") && primitive["attributes"].is_object()){
                for (auto it = primitive["attributes"].begin(); it != primitive["attributes"].end(); ++it){
                    if (it->is_number_unsigned() || (it->is_number_integer() && it->get<long long>() >= 0))
                        accessors.push_back(it->get<size_t>());
                }
            }
            size_t indices = 0;
            if (GltfDocument::getIndex(primitive, "indices", indices))
                accessors.push_back(indices);
            for (size_t a : accessors){
                if (moved.count(a))
                    continue;
                if (a >= model.accessors.size() || a >= root["accessors"].size()){
                    error = where + " names accessor " + std::to_string(a) + ", which the file does not have";
                    return false;
                }
                const tinygltf::Accessor& decoded = model.accessors[a];
                if (decoded.bufferView < 0 || decoded.bufferView >= (int)model.bufferViews.size()){
                    error = "the Draco stream of " + where + " left accessor " + std::to_string(a) + " without data";
                    return false;
                }
                const tinygltf::BufferView& view = model.bufferViews[(size_t)decoded.bufferView];
                if (view.buffer < 0 || view.buffer >= (int)model.buffers.size() ||
                        view.byteOffset + view.byteLength > model.buffers[(size_t)view.buffer].data.size()){
                    error = "the Draco stream of " + where + " decoded outside its buffer";
                    return false;
                }
                const std::vector<unsigned char>& data = model.buffers[(size_t)view.buffer].data;
                std::vector<uint8_t> bytes(data.begin() + (std::ptrdiff_t)view.byteOffset,
                                           data.begin() + (std::ptrdiff_t)(view.byteOffset + view.byteLength));
                const size_t added = doc.addView(std::move(bytes), view.byteStride, view.target);
                Json& accessor = root["accessors"][a];
                accessor["bufferView"] = added;
                if (decoded.byteOffset > 0)
                    accessor["byteOffset"] = decoded.byteOffset;
                else
                    accessor.erase("byteOffset");
                accessor.erase("sparse");
                moved[a] = added;
            }
            primitive["extensions"].erase(DRACO);
            if (primitive["extensions"].empty())
                primitive.erase("extensions");
        }
    }
    doc.pruneViews();
    removeFromList(root, "extensionsRequired", DRACO);
    removeFromList(root, "extensionsUsed", DRACO);
    return true;
}

bool ModelSourceDecode::transcodeImages(GltfDocument& doc, bool basisFallback, std::vector<std::string>& transcoded, std::string& error){
    Json& root = doc.json();
    if (!GltfDocument::arrayOf(root, "textures"))
        return true;
    doc.prepareViews();
    const Json* textures = GltfDocument::arrayOf(root, "textures");
    // the WebP images: every texture's EXT_texture_webp source, and images whose bytes are WebP
    std::set<size_t> webp;
    for (const Json& texture : *textures){
        if (!texture.is_object() || !texture.contains("extensions") || !texture["extensions"].is_object())
            continue;
        size_t image = 0;
        if (texture["extensions"].contains(WEBP) && GltfDocument::getIndex(texture["extensions"][WEBP], "source", image))
            webp.insert(image);
    }
    std::map<size_t, size_t> pngOf;     // WebP image -> its view
    for (size_t image : webp){
        std::vector<uint8_t> bytes;
        if (!doc.imageBytes(image, bytes, error))
            return false;
        if (!GltfNormalize::isWebp(bytes.data(), bytes.size())){
            error = "image " + std::to_string(image) + " is named by " + WEBP + " but is not WebP";
            return false;
        }
        size_t view = 0;
        if (!pngView(doc, bytes, "the WebP image " + std::to_string(image), view, error))
            return false;
        pngOf[image] = view;
    }
    for (const auto& [image, view] : pngOf){
        Json& object = root["images"][image];
        object.erase("uri");
        object["bufferView"] = view;
        object["mimeType"] = "image/png";
    }
    bool basisDone = false;
    for (Json& texture : root["textures"]){
        if (!texture.is_object() || !texture.contains("extensions") || !texture["extensions"].is_object())
            continue;
        // ordered_json keeps an object's members in a vector: the `source` added last, since adding
        // a key moves them, and a reference to "extensions" with them
        std::optional<size_t> source;
        {
            Json& extensions = texture["extensions"];
            size_t image = 0;
            if (extensions.contains(WEBP)){
                if (!texture.contains("source") && GltfDocument::getIndex(extensions[WEBP], "source", image))
                    source = image;
                extensions.erase(WEBP);
            }
            if (basisFallback && !source && extensions.contains(BASISU) && !texture.contains("source") && GltfDocument::getIndex(extensions[BASISU], "source", image)){
                // a native player reads `source`: the KTX2 image's pixels as a PNG image of its own
                std::vector<uint8_t> bytes;
                if (!doc.imageBytes(image, bytes, error))
                    return false;
                size_t view = 0;
                if (!pngView(doc, bytes, "the KTX2 image " + std::to_string(image), view, error))
                    return false;
                root["images"].push_back(Json{{"bufferView", view}, {"mimeType", "image/png"}});
                source = root["images"].size() - 1;
                basisDone = true;
            }
        }
        if (texture["extensions"].empty())
            texture.erase("extensions");
        if (source)
            texture["source"] = *source;
    }
    doc.pruneViews();
    if (!webp.empty()){
        removeFromList(root, "extensionsRequired", WEBP);
        removeFromList(root, "extensionsUsed", WEBP);
        transcoded.push_back(WEBP);
    }
    if (basisDone){
        // with a PNG source on every texture the extension is optional: a player without the
        // transcoder reads the source
        bool everyHasSource = true;
        for (const Json& texture : root["textures"]){
            if (texture.is_object() && texture.contains("extensions") && texture["extensions"].is_object() &&
                    texture["extensions"].contains(BASISU) && !texture.contains("source"))
                everyHasSource = false;
        }
        if (everyHasSource)
            removeFromList(root, "extensionsRequired", BASISU);
        transcoded.push_back(BASISU);
    }
    return true;
}

std::string ModelSourceDecode::blockingError(const std::vector<GltfDiagnostic>& diagnostics){
    for (const GltfDiagnostic& diagnostic : diagnostics){
        if (diagnostic.severity == GltfDiagnosticSeverity::Error && !diagnostic.cookFixes)
            return diagnostic.text();
    }
    return std::string();
}
