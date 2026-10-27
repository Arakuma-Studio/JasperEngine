// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureExport.h"

#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "texture/CookedTextures.h"

#include "json.hpp"

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

bool TextureExport::gather(Project& project, std::map<std::string, TextureCookInputs>& inputs, std::string& error){
    inputs.clear();
    for (const std::string& path : projectTextureSources(project, true)){
        TextureCookInputs in;
        if (!gatherTextureInputs(project, path, in)){
            std::string reason;
            TextureImportSettings ignored;
            TextureImportSettings::load(in.absolute.string(), ignored, &reason);
            error = path + ": its texture import settings cannot be read: " + reason;
            return false;
        }
        inputs.emplace(path, std::move(in));
    }
    return true;
}

bool TextureExport::writeFamily(TextureCookCache& cache, TextureFamily family, const std::vector<TextureBatchItem>& items,
                                const fs::path& assetsOut, std::string& error){
    const fs::path dir = assetsOut / CookedTextures::kCookedDir / textureFamilyName(family);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec){
        error = "cannot create " + dir.generic_string() + ": " + ec.message();
        return false;
    }
    nlohmann::json textures = nlohmann::json::object();
    std::set<std::string> written;
    for (const TextureBatchItem& item : items){
        if (item.family != family || item.sourceOnly || !item.result.ok)
            continue;
        const TextureCookRecord& record = item.result.record;
        const std::string file = record.key + ".ktx2";
        const std::string cpuFile = record.cpuBytes > 0 ? record.key + ".cpu.ktx2" : std::string();
        // identical cooks (duplicated sources) share one file
        if (written.insert(record.key).second){
            for (const std::string& name : {file, cpuFile}){
                if (name.empty())
                    continue;
                std::vector<uint8_t> bytes;
                const char* suffix = name == file ? ".ktx2" : ".cpu.ktx2";
                if (!cache.readArtifact(record.key, bytes, suffix)){
                    error = item.assetPath + ": the cooked " + textureFamilyName(family) + " file is missing from the texture cache";
                    return false;
                }
                std::string writeError;
                if (!assetio::writeFile(dir / name, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                        assetio::Durability::Cache, &writeError)){
                    error = "cannot write " + (dir / name).generic_string() + ": " + writeError;
                    return false;
                }
            }
        }
        nlohmann::json flags = nlohmann::json::array();
        const std::string text = TextureCooker::flagsText(record.flags);
        size_t start = 0;
        while (start < text.size()){
            const size_t comma = text.find(',', start);
            flags.push_back(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        nlohmann::json entry = {{"file", file}, {"width", record.logicalWidth}, {"height", record.logicalHeight},
                                {"format", record.format}, {"flags", flags}, {"levels", record.levels}, {"faces", record.faces},
                                {"bytes", record.diskBytes}, {"tail", record.tailBytes}};
        if (!cpuFile.empty())
            entry["cpu"] = cpuFile;
        textures[item.assetPath] = entry;
    }
    const std::string index = nlohmann::json({{"version", 1}, {"textures", textures}}).dump(1) + "\n";
    std::string writeError;
    if (!assetio::writeFile(dir / "index.json", index, assetio::Durability::Cache, &writeError)){
        error = "cannot write " + (dir / "index.json").generic_string() + ": " + writeError;
        return false;
    }
    return true;
}

TextureExportResult TextureExport::cookForExport(Project& project, const std::map<std::string, TextureCookInputs>& inputs,
                                                 const std::vector<TextureFamily>& families, const fs::path& assetsOut,
                                                 int threads, const std::function<void(int, int, const std::string&)>& progress,
                                                 const std::function<bool()>& cancelled){
    TextureExportResult out;
    // the editor's cache when the queue has this project open (one source table), else its own
    TextureCookCache local;
    TextureCookQueue& queue = TextureCookQueue::get();
    const bool shared = queue.getProject() == &project;
    TextureCookCache& cache = shared ? queue.getCache() : local;
    const TextureProjectSettings settings = textureProjectSettingsOf(project);
    if (!shared)
        local.open(project.getProjectInternalPath() / "cache" / "textures" / "v1", settings.globalCacheDir,
                   (uint64_t)std::max(settings.cacheSizeMB, 64) << 20);

    std::vector<TextureBatchItem> items;
    for (const auto& [path, in] : inputs){
        for (TextureFamily family : families){
            TextureBatchItem item;
            item.assetPath = path;
            item.family = family;
            items.push_back(std::move(item));
        }
    }
    TextureBatchCook::run(cache, settings, inputs, items, threads, shared ? queue.getMemoryBudget() : (2ull << 30), progress, cancelled);
    if (cancelled && cancelled()){
        out.error = "cancelled";
        return out;
    }

    std::map<std::string, TextureReportRow> rows;
    std::map<std::string, bool> allCooked;
    for (const TextureBatchItem& item : items){
        const TextureCookInputs& in = inputs.at(item.assetPath);
        TextureReportRow& row = rows[item.assetPath];
        // project-relative, as MCP's report names them
        row.path = in.projectRelative.empty() ? item.assetPath : in.projectRelative;
        row.type = in.settings.resolvedType();
        row.inferredFrom = in.settings.inferredFrom;
        row.families[item.family] = TextureCookReport::stateOf(item, &in);
        if (!item.result.ok && out.error.empty())
            out.error = item.assetPath + " (" + textureFamilyName(item.family) + "): " + item.result.error;
        auto [it, inserted] = allCooked.emplace(item.assetPath, true);
        it->second = it->second && item.result.ok && !item.sourceOnly;
        if (item.result.ok && !item.sourceOnly){
            if (item.result.cacheHit)
                out.cacheHits++;
            else
                out.cooked++;
        }
    }
    for (auto& [path, row] : rows)
        out.rows.push_back(std::move(row));
    if (!out.error.empty())
        return out;

    if (!assetsOut.empty()){
        for (TextureFamily family : families){
            if (!writeFamily(cache, family, items, assetsOut, out.error))
                return out;
        }
        for (const auto& [path, cooked] : allCooked){
            const TextureCookInputs& in = inputs.at(path);
            if (cooked && !in.settings.shipSource.value_or(false) && !in.inference.readsSource)
                out.cookedSources.insert(path);
        }
    }
    if (!shared){
        local.sweep();
        local.close();
    }
    out.ok = true;
    return out;
}
