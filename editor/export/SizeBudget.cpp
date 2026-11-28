// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SizeBudget.h"

#include "cook/BuildReport.h"

#include <algorithm>
#include <atomic>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::atomic<uint64_t>& budgetEvaluations(){
        static std::atomic<uint64_t> count{0};
        return count;
    }

    // how closely a row names the build's platform: 0 does not apply
    int budgetSpecificity(const std::string& row, const std::string& platform){
        if (row == platform) return 3;
        if (row == "desktop" && (platform == "windows" || platform == "linux" || platform == "macos")) return 2;
        if (row == "all") return 1;
        return 0;
    }

}

const std::vector<std::string>& sizebudget::platformNames(){
    static const std::vector<std::string> names = {"all", "windows", "linux", "macos", "android", "ios", "web", "desktop"};
    return names;
}

const std::vector<std::string>& sizebudget::categoryNames(){
    static const std::vector<std::string> names = {"total", "models", "textures", "audio", "scripts", "scenes", "engine", "other"};
    return names;
}

bool sizebudget::isContentCategory(const std::string& category){
    return category != "total" && category != "engine";
}

uint64_t sizebudget::limitBytes(double limitMb){
    return (uint64_t)std::llround(limitMb * 1048576.0);
}

mcp::Json sizebudget::toJson(const std::vector<SizeBudgetRow>& rows){
    mcp::Json list = mcp::Json::array();
    for (const SizeBudgetRow& row : rows) list.push_back({{"platform", row.platform}, {"category", row.category}, {"limit_mb", row.limitMb}});
    return list;
}

bool sizebudget::fromJson(const mcp::Json& value, std::vector<SizeBudgetRow>& rows, std::string& error){
    const std::string shape = "must be a list of {platform, category, limit_mb}: platform all, windows, linux, macos, android, ios, web or "
                              "desktop; category total, models, textures, audio, scripts, scenes, engine or other; limit_mb above 0";
    if (!value.is_array()){
        error = shape;
        return false;
    }
    std::vector<SizeBudgetRow> out;
    for (const mcp::Json& item : value){
        SizeBudgetRow row;
        if (!item.is_object() || !item.contains("limit_mb") || !item["limit_mb"].is_number()){
            error = shape;
            return false;
        }
        row.platform = item.value("platform", std::string("all"));
        row.category = item.value("category", std::string("total"));
        row.limitMb = item["limit_mb"].get<double>();
        const auto& platforms = platformNames();
        const auto& categories = categoryNames();
        if (std::find(platforms.begin(), platforms.end(), row.platform) == platforms.end() ||
            std::find(categories.begin(), categories.end(), row.category) == categories.end() || !(row.limitMb > 0.0) ||
            !std::isfinite(row.limitMb)){
            error = shape + " (got " + item.dump() + ")";
            return false;
        }
        out.push_back(row);
    }
    rows = std::move(out);
    return true;
}

std::vector<SizeBudgetRow> sizebudget::applicable(const std::vector<SizeBudgetRow>& rows, const std::string& platform){
    std::vector<SizeBudgetRow> out;
    for (const std::string& category : categoryNames()){
        const SizeBudgetRow* best = nullptr;
        int bestRank = 0;
        for (const SizeBudgetRow& row : rows){
            if (row.category != category) continue;
            const int rank = budgetSpecificity(row.platform, platform);
            if (rank > bestRank){
                best = &row;
                bestRank = rank;
            }
        }
        if (best) out.push_back(*best);
    }
    return out;
}

std::vector<BuildReportBudget> sizebudget::evaluate(const std::vector<SizeBudgetRow>& rows, const BuildReport& report, bool content){
    std::vector<BuildReportBudget> out;
    if (rows.empty() || report.method == "source" || report.platform == "source") return out;
    budgetEvaluations()++;
    for (const SizeBudgetRow& row : applicable(rows, report.platform)){
        if (isContentCategory(row.category) != content) continue;
        BuildReportBudget budget;
        budget.platform = row.platform;
        budget.category = row.category;
        budget.limitBytes = limitBytes(row.limitMb);
        if (row.category == "total") {
            budget.bytes = report.totalBytes;
        } else {
            const auto it = report.categories.find(row.category);
            budget.bytes = it == report.categories.end() ? 0 : it->second;
        }
        out.push_back(budget);
    }
    return out;
}

uint64_t sizebudget::evaluations(){
    return budgetEvaluations().load();
}

std::string sizebudget::overText(const BuildReportBudget& budget, const BuildReport& report){
    std::vector<const BuildReportFile*> largest;
    for (const BuildReportFile& file : report.files)
        if (budget.category == "total" || file.category == budget.category) largest.push_back(&file);
    const size_t shown = std::min<size_t>(3, largest.size());
    std::partial_sort(largest.begin(), largest.begin() + (long)shown, largest.end(), [](const BuildReportFile* a, const BuildReportFile* b){
        return a->bytes != b->bytes ? a->bytes > b->bytes : a->path < b->path;
    });
    std::string files;
    for (size_t i = 0; i < shown; i++)
        files += (i ? ", " : "") + (largest[i]->source.empty() ? largest[i]->path : largest[i]->source) + " " + BuildReport::formatBytes(largest[i]->bytes);
    return report.platform + " " + budget.category + " is " + BuildReport::formatBytes(budget.bytes) + ", over its " +
           BuildReport::formatBytes(budget.limitBytes) + " budget (" + budget.platform + ") by " + BuildReport::formatBytes(budget.overBytes()) +
           (files.empty() ? std::string() : "; largest: " + files) +
           ": shrink them, or change the budget in Project Settings > Player > Size Budgets";
}
