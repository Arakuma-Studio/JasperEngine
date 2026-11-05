// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelExport.h"

#include "Project.h"
#include "htmlui/compat/Hash.h"
#include "io/AtomicFile.h"
#include "model/ModelHeader.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

bool ModelExport::gather(Project& project, const std::vector<ModelFamily>& families, ModelExportInputs& inputs, std::string& error){
    inputs = ModelExportInputs();
    ModelUsageIndex& usage = ModelUsageIndex::get();
    usage.refresh(project);
    inputs.scripts = project.getModelSettings().scriptMeshData;
    inputs.scriptReaders = usage.scriptReaders();
    for (const std::string& assetPath : projectModelSources(project)){
        for (ModelFamily family : families){
            ModelBatchItem item;
            item.assetPath = assetPath;
            item.family = family;
            if (!gatherModelCookRequest(project, assetPath, family, nullptr, item.request, error))
                return false;
            inputs.items.push_back(std::move(item));
        }
        inputs.usage[assetPath] = usage.usageOf(assetPath);
        const fs::path absolute = project.getAssetsPath() / FileUtils::pathFromUtf8(assetPath);
        const fs::path read = FbxImporter::isFbxFile(absolute) ? project.getImportedModelPath(absolute) : absolute;
        ModelHeader header;
        std::string headerError;
        inputs.skinned[assetPath] = readModelHeader(read, header, headerError) && header.skinned;
        // a .gltf's own .bin files (an FBX's import is not in the assets)
        if (!FbxImporter::isFbxFile(absolute)){
            for (const std::string& buffer : header.buffers){
                const fs::path file = (absolute.parent_path() / FileUtils::pathFromUtf8(buffer)).lexically_normal();
                if (project.isInsideAssetsPath(file))
                    inputs.extraFiles[assetPath].push_back(project.normalizeToAssetsRelative(file).generic_string());
            }
        }
    }
    return true;
}

ModelExportResult ModelExport::cookForExport(Project& project, ModelExportInputs& inputs, const fs::path& assetsOut, int threads,
                                             const std::function<void(int, int, const std::string&)>& progress,
                                             const std::function<bool()>& cancelled){
    ModelExportResult result;
    ModelCookCache cache;
    ModelCookQueue& queue = ModelCookQueue::get();
    // the editor's cache when it is this project's (an export run by the CLI opens its own)
    ModelCookCache* useCache = queue.getProject() == &project ? &queue.getCache() : &cache;
    if (useCache == &cache)
        cache.open(project.getProjectInternalPath() / "cache" / "models" / "v1", (uint64_t)std::max(project.getModelSettings().cacheSizeMB, 64) << 20);
    ModelBatchCook::run(*useCache, inputs.items, threads, progress, cancelled);
    if (cancelled && cancelled()){
        result.error = "cancelled";
        return result;
    }
    std::map<std::string, ModelExportRow> rows;
    for (ModelBatchItem& item : inputs.items){
        ModelExportRow& row = rows[item.assetPath];
        row.assetPath = item.assetPath;
        if (!item.result.ok){
            if (result.error.empty())
                result.error = item.assetPath + " (" + modelFamilyName(item.family) + "): " + item.result.error;
        }else{
            item.result.cacheHit ? result.cacheHits++ : result.cooked++;
            const bool baked = item.result.record.lodPrimitives > 0;
            row.readWrite[item.family] = ModelUsageIndex::decide(inputs.usage[item.assetPath], inputs.scriptReaders, item.request.settings.readWrite,
                                                                 inputs.scripts, inputs.skinned[item.assetPath], baked);
        }
        row.families[item.family] = item.result;
    }
    for (auto& [path, row] : rows)
        result.rows.push_back(row);
    if (!result.error.empty())
        return result;

    if (!assetsOut.empty()){
        const fs::path dir = assetsOut / CookedModels::kCookedDir;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        nlohmann::json index = {{"version", 1}, {"format", CookedModels::kFormatVersion}, {"models", nlohmann::json::object()}};
        std::set<std::string> written;
        for (ModelExportRow& row : result.rows){
            bool shipSource = false;
            nlohmann::json families = nlohmann::json::object();
            for (const auto& [family, cooked] : row.families){
                std::vector<uint8_t> bytes;
                if (!useCache->readArtifact(cooked.key, bytes)){
                    result.error = "the cook of " + row.assetPath + " went from the cache before it was copied: export again";
                    return result;
                }
                const std::string file = htmlui::compat::sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())) + ".glb";
                row.files[family] = file;
                if (!written.count(file)){
                    io::AtomicWriteOptions options;
                    options.durability = io::Durability::Cache;
                    const io::WriteResult write = io::writeFileAtomic(dir / file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options);
                    if (!write.ok()){
                        result.error = "cannot write the cook of " + row.assetPath + ": " + write.message;
                        return result;
                    }
                    written.insert(file);
                }
                const auto decision = row.readWrite.find(family);
                families[modelFamilyName(family)] = {{"file", file}, {"bytes", cooked.record.diskBytes}, {"sourceBytes", cooked.record.sourceBytes},
                                                    {"keepCpuData", decision == row.readWrite.end() || decision->second.keep}};
            }
            for (const ModelBatchItem& item : inputs.items)
                if (item.assetPath == row.assetPath)
                    shipSource = shipSource || item.request.settings.shipSource;
            index["models"][row.assetPath] = families;
            if (!shipSource){
                result.cookedSources.insert(row.assetPath);
                for (const std::string& extra : inputs.extraFiles[row.assetPath])
                    result.cookedSources.insert(extra);
            }
        }
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        const io::WriteResult write = io::writeFileAtomic(dir / "index.json", index.dump(1), options);
        if (!write.ok()){
            result.error = "cannot write " + (dir / "index.json").generic_string() + ": " + write.message;
            return result;
        }
        // a .bin another (shipped) model reads stays
        for (const auto& [model, extras] : inputs.extraFiles){
            if (result.cookedSources.count(model))
                continue;
            for (const std::string& extra : extras)
                result.cookedSources.erase(extra);
        }
    }
    result.ok = true;
    return result;
}

std::string ModelExport::toText(const ModelExportResult& result){
    std::ostringstream out;
    for (const ModelExportRow& row : result.rows){
        for (const auto& [family, cooked] : row.families){
            if (!cooked.ok)
                continue;
            const ModelCookRecord& r = cooked.record;
            char line[256];
            std::snprintf(line, sizeof(line), "Model %s [%s]: %lld tris, %.2f MB -> %.2f MB%s%s", row.assetPath.c_str(), modelFamilyName(family),
                          r.after.triangles, r.sourceBytes / 1048576.0, r.diskBytes / 1048576.0,
                          r.lods.empty() ? "" : (", " + std::to_string(r.lods.size()) + " LODs").c_str(),
                          cooked.cacheHit ? " (cached)" : "");
            out << line << "\n";
        }
    }
    return out.str();
}
