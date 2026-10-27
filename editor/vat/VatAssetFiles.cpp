// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatAssetFiles.h"

#include "animation/VertexAnimation.h"
#include "component/ModelComponent.h"
#include "registry/EntityRegistry.h"
#include "util/Util.h"
#include "vat/VatAssetWriter.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

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
