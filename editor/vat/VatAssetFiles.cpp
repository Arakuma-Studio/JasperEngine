// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatAssetFiles.h"

#include "animation/VertexAnimation.h"
#include "component/ModelComponent.h"
#include "registry/EntityRegistry.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "vat/VatAssetWriter.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>

namespace fs = std::filesystem;
using namespace jasperengine;

namespace{

    bool endsWith(const std::string& text, const std::string& suffix){
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string normalized(const std::string& path){
        std::string normal = fs::path(path).lexically_normal().generic_string();
        while (normal.compare(0, 2, "./") == 0) normal.erase(0, 2);
        return normal;
    }

    // ---- GLB chunks ----

    constexpr uint32_t GLB_MAGIC = 0x46546C67u;       // "glTF"
    constexpr uint32_t GLB_CHUNK_JSON = 0x4E4F534Au;  // "JSON"

    uint32_t readU32(const std::string& bytes, size_t at){
        uint32_t value = 0;
        for (int i = 3; i >= 0; i--) value = (value << 8) | (uint8_t)bytes[at + (size_t)i];
        return value;
    }

    void writeU32(std::string& bytes, size_t at, uint32_t value){
        for (int i = 0; i < 4; i++) bytes[at + (size_t)i] = (char)((value >> (8 * i)) & 0xFFu);
    }

    bool readBytes(const fs::path& path, std::string& out){
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !file.bad();
    }

    // the JSON chunk's place in a GLB's bytes: [start, start + length)
    bool jsonChunk(const std::string& bytes, size_t& start, size_t& length){
        if (bytes.size() < 20 || readU32(bytes, 0) != GLB_MAGIC || readU32(bytes, 4) != 2) return false;
        length = readU32(bytes, 12);
        if (readU32(bytes, 16) != GLB_CHUNK_JSON || length > bytes.size() - 20) return false;
        start = 20;
        return true;
    }

    // ---- URIs ----

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

    int hexValue(char c){
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::string decodeUri(const std::string& uri){
        std::string out;
        for (size_t i = 0; i < uri.size(); i++){
            if (uri[i] == '%' && i + 2 < uri.size() && hexValue(uri[i + 1]) >= 0 && hexValue(uri[i + 2]) >= 0){
                out += (char)(hexValue(uri[i + 1]) * 16 + hexValue(uri[i + 2]));
                i += 2;
            }else{
                out += uri[i];
            }
        }
        return out;
    }

    // a URI naming a file relative to the glb (no data:, no scheme, not absolute)
    bool relativeFileUri(const std::string& uri){
        if (uri.empty() || uri[0] == '/' || uri[0] == '\\') return false;
        const size_t colon = uri.find(':');
        const size_t slash = uri.find('/');
        return colon == std::string::npos || (slash != std::string::npos && slash < colon);
    }

    // `path` inside (or equal to) `folder`: the part below it
    bool below(const fs::path& path, const fs::path& folder, fs::path& rest){
        const fs::path relative = path.lexically_normal().lexically_relative(folder.lexically_normal());
        if (relative.empty() || *relative.begin() == "..") return false;
        rest = relative;
        return true;
    }

    // where `path` went in a step that moved `moved` (old -> new): itself when it didn't go
    fs::path followed(const fs::path& path, const std::vector<std::pair<fs::path, fs::path>>& moved, bool backwards){
        for (const auto& entry : moved){
            const fs::path& from = backwards ? entry.second : entry.first;
            const fs::path& to = backwards ? entry.first : entry.second;
            fs::path rest;
            if (below(path, from, rest)){
                return rest == "." ? to.lexically_normal() : (to / rest).lexically_normal();
            }
        }
        return path.lexically_normal();
    }

}

bool editor::vat::isVatMeshFile(const std::string& path){
    return endsWith(path, jasperengine::vat::VAT_MESH_SUFFIX) && fs::path(path).filename().string().size() > 8;
}

fs::path editor::vat::pairPathOf(const fs::path& path){
    const std::string text = path.generic_string();
    if (isVatMeshFile(text)){
        return fs::path(text.substr(0, text.size() - 4));   // drop ".glb"
    }
    if (Util::isVertexAnimationFile(text) && !path.stem().empty()){
        return fs::path(text + ".glb");
    }
    return fs::path();
}

fs::path editor::vat::existingPairOf(const fs::path& path){
    const fs::path pair = pairPathOf(path);
    std::error_code ec;
    if (pair.empty() || !fs::is_regular_file(pair, ec) || !fs::is_regular_file(path, ec)){
        return fs::path();
    }
    return pair;
}

std::vector<fs::path> editor::vat::withPairs(const std::vector<fs::path>& paths){
    std::vector<fs::path> out = paths;
    auto listed = [&out](const fs::path& path){
        const fs::path normal = path.lexically_normal();
        return std::any_of(out.begin(), out.end(), [&normal](const fs::path& other){ return other.lexically_normal() == normal; });
    };
    for (const fs::path& path : paths){
        const fs::path pair = existingPairOf(path);
        if (!pair.empty() && !listed(pair)){
            out.push_back(pair);
        }
    }
    return out;
}

bool editor::vat::renamedPair(const fs::path& oldPath, const fs::path& newPath, fs::path& pairOld, fs::path& pairNew){
    pairOld.clear();
    pairNew.clear();
    const bool vatFile = Util::isVertexAnimationFile(oldPath.string()) && Util::isVertexAnimationFile(newPath.string());
    const bool meshFile = isVatMeshFile(oldPath.generic_string()) && isVatMeshFile(newPath.generic_string());
    if (!vatFile && !meshFile){
        return false;
    }
    const fs::path from = pairPathOf(oldPath);
    const fs::path to = pairPathOf(newPath);
    std::error_code ec;
    if (from.empty() || to.empty() || from == to || !fs::is_regular_file(from, ec)){
        return false;
    }
    pairOld = from;
    pairNew = to;
    return true;
}

bool editor::vat::isVatBakeSource(const fs::path& path){
    const std::string text = path.generic_string();
    if (isVatMeshFile(text)) return false;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return extension == ".gltf" || extension == ".glb" || extension == ".fbx" || Util::isAnimationClipFile(text);
}

bool editor::vat::isVatImportSource(const fs::path& path){
    const std::string name = path.filename().string();
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (endsWith(lower, "-remap_info.json") || endsWith(lower, "_data.json")){
        return true;
    }
    std::error_code ec;
    const fs::path dir = path.parent_path();
    if (endsWith(lower, "_mat.mat")){
        return fs::is_directory(dir / "geo", ec) || fs::is_directory(dir.parent_path() / "geo", ec);
    }
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (extension != ".fbx" && extension != ".glb" && extension != ".gltf" && extension != ".obj"){
        return false;
    }
    if (isVatMeshFile(path.generic_string())) return false;
    const std::string stem = path.stem().string();
    auto anyOf = [&ec](const fs::path& folder, const std::string& base, std::initializer_list<const char*> extensions){
        for (const char* ext : extensions){
            if (fs::is_regular_file(folder / (base + ext), ec)) return true;
        }
        return false;
    };
    // OpenVAT: "<name>_vat.exr" next to "<name>.fbx"
    if (anyOf(dir, stem + "_vat", {".exr", ".png"})) return true;
    // Houdini VAT 3: "geo/<asset>_mesh.fbx" with "tex/<asset>_pos.exr"
    const std::string asset = endsWith(stem, "_mesh") ? stem.substr(0, stem.size() - 5) : stem;
    return anyOf(dir, asset + "_pos", {".exr", ".png", ".tga"}) || anyOf(dir.parent_path() / "tex", asset + "_pos", {".exr", ".png", ".tga"});
}

bool editor::vat::setMeshField(std::string& text, const std::string& mesh){
    jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(text);
    if (!parsed.ok() || parsed.data->mesh == mesh){
        return false;
    }
    parsed.data->mesh = mesh;
    text = jasperengine::vat::writeVat(*parsed.data);
    return true;
}

bool editor::vat::followVatMesh(EntityRegistry* registry, Entity entity, const std::string& oldVat, const std::string& newVat,
                                 bool fillEmpty){
    if (!registry || newVat.empty() || !Util::isVertexAnimationFile(newVat)){
        return false;
    }
    ModelComponent* model = registry->findComponent<ModelComponent>(entity);
    if (!model){
        return false;
    }
    const std::string newMesh = normalized(meshPathFor(newVat));
    const std::string current = normalized(model->filename);
    const bool playedOld = !oldVat.empty() && Util::isVertexAnimationFile(oldVat) && current == normalized(meshPathFor(oldVat));
    if (current == newMesh || !(playedOld || (fillEmpty && model->filename.empty()))){
        return false;
    }
    model->filename = newMesh;
    model->needUpdateModel = true;
    return true;
}

std::string editor::vat::pairRenameRefusal(const fs::path& from, const fs::path& to, const std::string& fromName){
    fs::path pairFrom, pairTo;
    if (existingPairOf(from).empty() || renamedPair(from, to, pairFrom, pairTo)){
        return {};
    }
    return fromName + (Util::isVertexAnimationFile(from.string())
                           ? " is a Vertex Animation: its new name must end in .vat (its mesh follows as <name>.vat.glb)."
                           : " is the mesh of a Vertex Animation: its new name must end in .vat.glb (its .vat follows).");
}

bool editor::vat::readGlbJson(const fs::path& glb, std::string& json){
    std::string bytes;
    size_t start = 0, length = 0;
    if (!readBytes(glb, bytes) || !jsonChunk(bytes, start, length)){
        return false;
    }
    json = bytes.substr(start, length);
    return true;
}

bool editor::vat::writeGlbJson(const fs::path& glb, const std::string& json){
    std::string bytes;
    size_t start = 0, length = 0;
    if (!readBytes(glb, bytes) || !jsonChunk(bytes, start, length)){
        return false;
    }
    std::string chunk = json;
    while (chunk.size() % 4 != 0) chunk += ' ';
    if (chunk.size() > 0xFFFFFFF0u) return false;
    // the header, the new JSON chunk, then every following chunk (BIN) byte for byte
    std::string out = bytes.substr(0, 20);
    out += chunk;
    out += bytes.substr(start + length);
    if (out.size() > 0xFFFFFFFFu) return false;
    writeU32(out, 8, (uint32_t)out.size());
    writeU32(out, 12, (uint32_t)chunk.size());
    return FileUtils::writeFileReplacing(glb, out);
}

bool editor::vat::rebaseVatMeshUris(const fs::path& glb, const fs::path& oldDirectory,
                                     const std::vector<std::pair<fs::path, fs::path>>& along,
                                     std::string& before, std::string& after){
    if (!readGlbJson(glb, before)){
        return false;
    }
    nlohmann::json gltf = nlohmann::json::parse(before, nullptr, false);
    if (gltf.is_discarded() || !gltf.is_object() || !gltf.contains("images") || !gltf["images"].is_array()){
        return false;
    }
    const fs::path newDirectory = glb.parent_path().lexically_normal();
    const fs::path oldDir = oldDirectory.lexically_normal();
    bool changed = false;
    for (nlohmann::json& image : gltf["images"]){
        if (!image.is_object() || !image.contains("uri") || !image["uri"].is_string()) continue;   // embedded
        const std::string uri = image["uri"].get<std::string>();
        if (!relativeFileUri(uri)) continue;
        const fs::path file = (oldDir / FileUtils::pathFromUtf8(decodeUri(uri))).lexically_normal();
        const fs::path target = followed(file, along, false);
        const fs::path relative = target.lexically_relative(newDirectory);
        if (relative.empty()) continue;     // another drive or root: can't be named relatively
        const std::string rebased = encodeUri(FileUtils::pathToGenericUtf8(relative));
        if (rebased != uri){
            image["uri"] = rebased;
            changed = true;
        }
    }
    if (!changed){
        return false;
    }
    after = gltf.dump();
    if (!writeGlbJson(glb, after)){
        return false;
    }
    // what the file holds now (the padding included), for the undo's comparison
    return readGlbJson(glb, after);
}

std::vector<fs::path> editor::vat::vatMeshesAt(const fs::path& path){
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)){
        if (isVatMeshFile(path.generic_string())) out.push_back(path);
        return out;
    }
    if (!fs::is_directory(path, ec)){
        return out;
    }
    for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)){
        if (it->is_regular_file(ec) && isVatMeshFile(it->path().generic_string())) out.push_back(it->path());
    }
    return out;
}

std::vector<editor::vat::VatUriEdit> editor::vat::rebaseMovedVatMeshes(const std::vector<std::pair<fs::path, fs::path>>& moved){
    std::vector<VatUriEdit> edits;
    for (const auto& [from, to] : moved){
        for (const fs::path& glb : vatMeshesAt(to)){
            const fs::path oldPlace = followed(glb, moved, true);
            if (oldPlace.parent_path() == glb.parent_path().lexically_normal()) continue;   // same directory
            VatUriEdit edit;
            edit.file = glb.lexically_normal();
            if (rebaseVatMeshUris(glb, oldPlace.parent_path(), moved, edit.before, edit.after)){
                edits.push_back(edit);
            }
        }
    }
    return edits;
}

void editor::vat::restoreVatMeshUris(const std::vector<VatUriEdit>& edits, const std::vector<std::pair<fs::path, fs::path>>& moved){
    for (const VatUriEdit& edit : edits){
        const fs::path file = followed(edit.file, moved, true);
        std::string now;
        // edited since (a rebake wrote another mesh): left as it is
        if (readGlbJson(file, now) && now == edit.after){
            writeGlbJson(file, edit.before);
        }
    }
}
