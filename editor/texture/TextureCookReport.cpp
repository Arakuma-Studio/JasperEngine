// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureCookReport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using mcp::Json;

TextureCookState TextureCookReport::stateOf(const TextureBatchItem& item, const TextureCookInputs* inputs){
    TextureCookState state;
    state.plan = item.result.plan;
    if (item.sourceOnly){
        state.status = TextureCookStatus::Source;
    }else if (item.result.ok){
        state.status = TextureCookStatus::Cooked;
        state.cacheHit = item.result.cacheHit;
        state.record = item.result.record;
    }else{
        state.status = item.result.cancelled ? TextureCookStatus::None : TextureCookStatus::Failed;
        state.error = item.result.error;
    }
    if (inputs){
        state.warnings = TextureTypeInference::conflicts(inputs->settings.resolvedType(), inputs->inference);
        state.problems = state.warnings;
    }
    state.problems.insert(state.problems.end(), item.result.plan.choice.problems.begin(), item.result.plan.choice.problems.end());
    for (const std::string& warning : item.result.plan.choice.warnings)
        state.warnings.push_back(warning);
    for (const std::string& warning : item.result.record.warnings)
        if (std::find(state.warnings.begin(), state.warnings.end(), warning) == state.warnings.end())
            state.warnings.push_back(warning);
    return state;
}

Json TextureCookReport::familyJson(const TextureCookState& state){
    Json out = Json::object();
    out["status"] = state.stale ? "stale" : textureCookStatusName(state.status);
    if (!state.error.empty())
        out["error"] = state.error;
    if (!state.plan.key.empty()){
        out["format"] = state.plan.choice.label();
        out["cpu_readable"] = state.plan.resolved.cpuReadable;
    }
    if (state.status == TextureCookStatus::Cooked){
        const TextureCookRecord& r = state.record;
        out["format"] = r.format;
        out["label"] = r.label;
        out["key"] = r.key;     // the cache's file: .jasperengine/cache/textures/v1/<key[0:2]>/<key>.ktx2
        out["width"] = r.width;
        out["height"] = r.height;
        out["logical_width"] = r.logicalWidth;
        out["logical_height"] = r.logicalHeight;
        out["mips"] = r.levels;
        out["faces"] = r.faces;
        out["vram_bytes"] = r.gpuBytes;
        out["disk_bytes"] = r.diskBytes;
        out["source_bytes"] = r.sourceBytes;
        if (r.psnr > 0.0)
            out["psnr_db"] = std::round(r.psnr * 100.0) / 100.0;
        if (r.psnrAlpha > 0.0)
            out["psnr_alpha_db"] = std::round(r.psnrAlpha * 100.0) / 100.0;
        if (r.normalMeanError > 0.0){
            out["normal_mean_error_deg"] = std::round(r.normalMeanError * 1000.0) / 1000.0;
            out["normal_p99_error_deg"] = std::round(r.normalP99Error * 1000.0) / 1000.0;
        }
        if (r.hdrRelativeRmse > 0.0)
            out["hdr_relative_rmse"] = std::round(r.hdrRelativeRmse * 10000.0) / 10000.0;
        out["cache_hit"] = state.cacheHit;
        out["cook_ms"] = std::round(r.cookMs);
        out["flags"] = TextureCooker::flagsText(r.flags);
    }
    if (!state.warnings.empty())
        out["warnings"] = state.warnings;
    return out;
}

Json TextureCookReport::toJson(const std::vector<TextureReportRow>& rows){
    Json textures = Json::array();
    struct Totals {
        int textures = 0, cooked = 0, failed = 0, cacheHits = 0;
        uint64_t vram = 0, disk = 0, source = 0;
    };
    std::map<TextureFamily, Totals> totals;
    for (const TextureReportRow& row : rows){
        Json entry = {{"path", row.path}, {"type", textureImportTypeName(row.type)}};
        if (!row.inferredFrom.empty())
            entry["inferred_from"] = row.inferredFrom;
        Json platforms = Json::object();
        for (const auto& [family, state] : row.families){
            platforms[textureFamilyName(family)] = familyJson(state);
            Totals& t = totals[family];
            t.textures++;
            if (state.status == TextureCookStatus::Cooked){
                t.cooked++;
                t.cacheHits += state.cacheHit ? 1 : 0;
                t.vram += state.record.gpuBytes;
                t.disk += state.record.diskBytes;
                t.source += state.record.sourceBytes;
            }else if (state.status == TextureCookStatus::Failed){
                t.failed++;
            }
        }
        entry["platforms"] = platforms;
        textures.push_back(entry);
    }
    Json totalsJson = Json::object();
    for (const auto& [family, t] : totals){
        totalsJson[textureFamilyName(family)] = {{"textures", t.textures}, {"cooked", t.cooked}, {"failed", t.failed},
                                                  {"cache_hits", t.cacheHits}, {"vram_bytes", t.vram}, {"disk_bytes", t.disk},
                                                  {"source_bytes", t.source}};
    }
    return {{"textures", textures}, {"totals", totalsJson}};
}

std::string TextureCookReport::formatBytes(uint64_t bytes){
    char buffer[32];
    if (bytes >= (1ull << 20))
        snprintf(buffer, sizeof(buffer), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(buffer, sizeof(buffer), "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buffer, sizeof(buffer), "%llu B", (unsigned long long)bytes);
    return buffer;
}

std::string TextureCookReport::infoLine(const TextureCookState& state){
    if (state.status != TextureCookStatus::Cooked){
        std::string line = state.stale ? "stale" : textureCookStatusName(state.status);
        if (!state.error.empty())
            line += ": " + state.error;
        return line;
    }
    const TextureCookRecord& r = state.record;
    char head[96];
    snprintf(head, sizeof(head), "%d\xC3\x97%d \xC2\xB7 ", r.width, r.height);
    std::string line = head + r.label + " \xC2\xB7 " + std::to_string(r.levels) + (r.levels == 1 ? " mip" : " mips") +
                       " \xC2\xB7 VRAM " + formatBytes(r.gpuBytes) + " \xC2\xB7 Disk " + formatBytes(r.diskBytes) +
                       " (source " + formatBytes(r.sourceBytes) + ")";
    char tail[64];
    if (r.normalMeanError > 0.0){
        snprintf(tail, sizeof(tail), " \xC2\xB7 %.2f\xC2\xB0 mean error", r.normalMeanError);
        line += tail;
    }else if (r.hdrRelativeRmse > 0.0){
        snprintf(tail, sizeof(tail), " \xC2\xB7 RMSE %.3f", r.hdrRelativeRmse);
        line += tail;
    }else if (r.psnr > 0.0){
        snprintf(tail, sizeof(tail), " \xC2\xB7 PSNR %.1f dB", r.psnr);
        line += tail;
    }
    return line;
}

std::string TextureCookReport::toText(const std::vector<TextureReportRow>& rows){
    std::string out;
    for (const TextureReportRow& row : rows){
        for (const auto& [family, state] : row.families){
            out += row.path + " [" + textureFamilyName(family) + "] " + infoLine(state);
            if (state.status == TextureCookStatus::Cooked && state.cacheHit)
                out += " (cached)";
            out += "\n";
            for (const std::string& warning : state.warnings)
                out += "  warning: " + warning + "\n";
        }
    }
    const Json json = toJson(rows);
    for (const auto& [family, t] : json["totals"].items()){
        out += "total [" + family + "] " + std::to_string(t["cooked"].get<int>()) + "/" + std::to_string(t["textures"].get<int>()) +
               " cooked (" + std::to_string(t["cache_hits"].get<int>()) + " cached, " + std::to_string(t["failed"].get<int>()) +
               " failed) \xC2\xB7 VRAM " + formatBytes(t["vram_bytes"].get<uint64_t>()) + " \xC2\xB7 Disk " +
               formatBytes(t["disk_bytes"].get<uint64_t>()) + " (source " + formatBytes(t["source_bytes"].get<uint64_t>()) + ")\n";
    }
    return out;
}

std::vector<std::string> TextureCookReport::problems(const std::vector<TextureReportRow>& rows){
    std::vector<std::string> out;
    for (const TextureReportRow& row : rows){
        for (const auto& [family, state] : row.families){
            if (state.status == TextureCookStatus::Failed)
                out.push_back(row.path + " [" + textureFamilyName(family) + "]: " + state.error);
            for (const std::string& problem : state.problems)
                out.push_back(row.path + " [" + textureFamilyName(family) + "]: " + problem);
        }
    }
    return out;
}
