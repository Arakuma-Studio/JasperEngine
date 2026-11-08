// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipStatistics.h"

#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"

#include <fstream>
#include <sstream>

using namespace jasperengine;
using editor::FileUtils;

std::vector<anim::ProjectClip> anim::listProjectClips(const std::filesystem::path& projectRoot, std::vector<std::string>& notes,
                                                      const std::atomic<bool>* cancel){
    std::vector<ProjectClip> clips;
    for (const std::filesystem::path& file : editor::ProjectChecks::findProjectFiles(projectRoot, {".anim", ".gltf", ".glb", ".fbx"})){
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        const std::string display = editor::ProjectChecks::projectRelative(projectRoot, file);
        const std::string reference = FileUtils::pathToGenericUtf8(file.lexically_normal());
        if (!isModelFilePath(reference)){
            clips.push_back({display, reference});
            continue;
        }
        std::vector<ModelClipInfo> infos;
        std::string error;
        if (!listModelFileClips(reference, infos, &error)){
            notes.push_back(display + ": " + error);
            continue;
        }
        for (const ModelClipInfo& info : infos) clips.push_back({display + "#" + info.name, formatSubClipReference(reference, info.name)});
    }
    return clips;
}

anim::ClipStatRow anim::readClipStatRow(const ProjectClip& clip){
    ClipStatRow row;
    row.clip = clip;
    SubClipReference parts;
    const bool subClip = parseSubClipReference(clip.display, parts);
    const std::filesystem::path file = FileUtils::pathFromUtf8(subClip ? parts.model : clip.display);
    row.name = subClip ? parts.clip : FileUtils::pathToUtf8(file.stem());
    row.source = FileUtils::pathToUtf8(file.filename());
    ClipReportRequest request;
    request.reference = clip.reference;
    row.report = analyzeClip(request);
    if (!subClip){
        std::ifstream stream(FileUtils::pathFromUtf8(clip.reference), std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        const ClipParseResult parsed = parseClip(text.str());
        if (parsed.ok()){
            const ClipOptimizeRecord record = readClipOptimizeRecord(*parsed.data);
            if (record.present) row.maxErrorMm = record.maxErrorMm;
        }
    }
    return row;
}

bool anim::clipStatRowShown(const ClipStatRow& row, ClipStatFilter filter, uint64_t budgetBytes){
    switch (filter){
        case ClipStatFilter::Loaded: return row.loaded;
        case ClipStatFilter::Unused: return row.users == 0;
        case ClipStatFilter::OverBudget: return row.report.runtimeBytes > budgetBytes;
        case ClipStatFilter::All: break;
    }
    return true;
}

anim::ClipStatTotals anim::clipStatTotals(const std::vector<ClipStatRow>& rows){
    ClipStatTotals totals;
    for (const ClipStatRow& row : rows){
        totals.clips++;
        totals.memoryBytes += row.report.runtimeBytes;
        if (row.loaded) totals.loadedBytes += row.report.runtimeBytes;
        if (row.users == 0){
            totals.unusedClips++;
            totals.unusedBytes += row.report.runtimeBytes;
        }
    }
    return totals;
}
