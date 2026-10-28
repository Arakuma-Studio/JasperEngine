// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatReport.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::vat;
using Json = nlohmann::json;

namespace jasperengine::editor::vat{

namespace{

    // 3 decimals: reports are read by people and agents, not compared bit for bit
    double rounded(double value){
        if (!std::isfinite(value)) return 0.0;
        return std::round(value * 1000.0) / 1000.0;
    }

}

bool hasErrors(const std::vector<VatIssue>& issues){
    for (const VatIssue& issue : issues){
        if (issue.error) return true;
    }
    return false;
}

std::string issuesText(const std::vector<VatIssue>& issues, bool errorsOnly){
    std::string text;
    for (const VatIssue& issue : issues){
        if (errorsOnly && !issue.error) continue;
        if (!text.empty()) text += "\n";
        text += issue.kind + ": " + issue.message;
        if (!issue.fix.empty()) text += " (" + issue.fix + ")";
    }
    return text;
}

const char* textureFormatName(VatPrecision precision){
    return precision == VatPrecision::Float ? "rgba32f" : "rgba16f";
}

Json issueJson(const VatIssue& issue){
    Json out = {{"kind", issue.kind}, {"message", issue.message}, {"fix", issue.fix}};
    if (issue.error) out["error"] = true;
    if (!issue.fixes.empty()){
        Json fixes = Json::array();
        for (const VatFix& fix : issue.fixes){
            fixes.push_back({{"label", fix.label}, {"setting", fix.setting}, {"value", fix.value}});
        }
        out["fixes"] = fixes;
    }
    return out;
}

Json issuesJson(const std::vector<VatIssue>& issues){
    Json out = Json::array();
    for (const VatIssue& issue : issues) out.push_back(issueJson(issue));
    return out;
}

Json reportJson(const VatReport& report){
    Json clips = Json::array();
    for (const VatClipReport& clip : report.clips){
        clips.push_back({{"name", clip.name}, {"frames", clip.frames}, {"fps", rounded(clip.fps)}, {"bytes", clip.bytes}});
    }
    return {
        {"mode", modeName(report.mode)},
        {"mode_reason", report.modeReason},
        {"vertices", report.vertices},
        {"bones", report.bones},
        {"clips", clips},
        {"texture", {{"width", report.width}, {"height", report.height}, {"format", textureFormatName(report.precision)}}},
        {"bytes", report.bytes},
        {"max_error_mm", rounded(report.maxErrorMm)},
        {"mean_error_mm", rounded(report.meanErrorMm)},
        {"quantization_error_mm", rounded(report.quantizationErrorMm)},
        {"tolerance_mm", rounded(report.toleranceMm)},
        {"fetches_per_vertex", report.fetchesPerVertex},
        {"warnings", issuesJson(report.warnings)},
    };
}

}
