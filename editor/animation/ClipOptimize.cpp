// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipOptimize.h"

#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "io/FileData.h"
#include "pool/AnimationClipPool.h"
#include "util/FileUtils.h"
#include "util/SHA1.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace jasperengine;

namespace{

    bool readBytes(const std::filesystem::path& file, std::string& out){
        std::ifstream stream(file, std::ios::binary);
        if (!stream) return false;
        std::ostringstream text;
        text << stream.rdbuf();
        out = text.str();
        return stream.good() || stream.eof();
    }

    std::filesystem::path systemPath(const std::string& path){
        return editor::FileUtils::pathFromUtf8(FileData::getSystemPath(path));
    }

    double rounded(double value){
        return std::round(value * 1000.0) / 1000.0;
    }

    anim::ClipReport statsOf(const std::shared_ptr<const AnimationClip>& clip, const std::string& reference){
        anim::ClipReportRequest request;
        request.clip = clip;
        request.reference = reference;
        return anim::analyzeClip(request);
    }

    // The source as it plays, and its value form
    bool loadSource(const std::string& reference, std::shared_ptr<const AnimationClip>& clip, ClipData& data, std::string& error){
        if (anim::isSubClipReference(reference)){
            const bool wasLoaded = AnimationClipPool::get(reference) != nullptr;
            clip = AnimationClipPool::getOrLoad(reference, &error);
            if (!clip) return false;
            data = clip->toData();
            if (!wasLoaded){
                const std::shared_ptr<const AnimationClip> kept = clip;
                clip.reset();
                AnimationClipPool::releaseUnused(reference);
                clip = kept;
            }
            anim::SubClipReference parts;
            anim::parseSubClipReference(reference, parts);
            data.source.kind = "model";
            data.source.path = parts.model;
            data.source.name = parts.clip;
            if (data.previewModel.empty()) data.previewModel = parts.model;
            data.guid.clear();
            return true;
        }
        std::string text;
        if (!readBytes(systemPath(reference), text)){
            error = "can't read " + reference;
            return false;
        }
        anim::ClipParseResult parsed = anim::parseClip(text);
        if (!parsed.ok()){
            error = parsed.error.toString();
            return false;
        }
        clip = AnimationClip::build(parsed);
        data = *parsed.data;
        return true;
    }

}

std::string anim::clipRigModel(const std::string& reference, const ClipData& data){
    SubClipReference parts;
    if (parseSubClipReference(reference, parts)) return parts.model;
    if (data.source.kind == "model" && isModelFilePath(data.source.path)) return data.source.path;
    if (isModelFilePath(data.previewModel)) return data.previewModel;
    return std::string();
}

std::string anim::clipRigModelOf(const std::string& reference){
    if (isSubClipReference(reference)) return clipRigModel(reference, ClipData());
    std::string text;
    if (!readBytes(systemPath(reference), text)) return std::string();
    const ClipParseResult parsed = parseClip(text);
    return parsed.ok() ? clipRigModel(reference, *parsed.data) : std::string();
}

std::string anim::clipSourceSha1(const std::string& reference){
    SubClipReference parts;
    const std::string file = parseSubClipReference(reference, parts) ? parts.model : reference;
    std::string bytes;
    if (!readBytes(systemPath(file), bytes)) return std::string();
    return editor::SHA1::hash(bytes);
}

anim::ClipOptimizeResult anim::runClipOptimize(const ClipOptimizeRequest& request){
    ClipOptimizeResult result;
    result.displayName = request.displayName.empty() ? request.reference : request.displayName;
    result.toleranceMm = request.reduction.toleranceMm;
    result.skinDistanceMm = std::max(request.reduction.skinDistanceMm, kClipReductionSkinFloorMm);
    ClipData source;
    if (!loadSource(request.reference, result.source, source, result.error)){
        result.errorKind = "read_failed";
        result.fix = "check the path; get_asset_info lists a model's clips";
        return result;
    }
    ClipReductionResult reduced = reduceClip(source, *request.rig, request.reduction, request.cancel);
    if (!reduced.ok){
        result.errorKind = reduced.errorKind;
        result.error = reduced.error;
        if (result.errorKind == "unknown_joint") result.fix = "get_model_info lists the bones; a trailing * matches any rest";
        if (result.errorKind == "non_finite_key") result.fix = "re-export the clip from its source";
        return result;
    }
    result.effectiveToleranceMm = reduced.effectiveToleranceMm;
    result.keptAllKeys = reduced.keptAllKeys;
    result.eventsKept = reduced.clip.events.size();
    for (const AnimationClip::Track& track : result.source->getTracks()) clipTrackIsReducible(track, *request.rig, &result.skipped);

    // the file form, as it will be written, is what gets measured
    const std::shared_ptr<const AnimationClip> written = AnimationClip::build(parseClip(writeClip(reduced.clip)));
    result.measured = measureClipError(*result.source, *written, *request.rig, result.skinDistanceMm, request.cancel);
    if (request.cancel && request.cancel->load(std::memory_order_relaxed)){
        result.errorKind = "cancelled";
        result.error = "cancelled";
        return result;
    }
    if (!result.measured.measured){
        result.errorKind = "nothing_measured";
        result.error = "no track of " + result.displayName + " moves a bone of " + request.rig->source;
        result.fix = "pass on: the GameObject that plays it, or set the clip's Preview model to the model it was made on";
        return result;
    }
    ClipOptimizeRecord record;
    record.toleranceMm = result.toleranceMm;
    record.skinDistanceMm = result.skinDistanceMm;
    record.maxErrorMm = rounded(result.measured.maxErrorMm);
    if (!request.inPlace){
        record.sourceSha1 = clipSourceSha1(request.reference);
        record.source = result.displayName;
    }else{
        // what it replaces is gone once written; a later edit of the file itself is no staleness
        record.sourceSha1 = readClipOptimizeRecord(source).sourceSha1;
        record.source = readClipOptimizeRecord(source).source;
    }
    writeClipOptimizeRecord(reduced.clip, record);
    result.reduced = std::move(reduced.clip);
    result.before = statsOf(result.source, request.reference);
    result.after = statsOf(written, std::string());
    result.ok = true;
    return result;
}

bool anim::verifyWrittenClip(const std::filesystem::path& file, const ClipRig& rig, ClipOptimizeResult& result, std::string& error){
    std::string text;
    if (!readBytes(file, text)){
        error = "can't read " + editor::FileUtils::pathToUtf8(file) + " back";
        return false;
    }
    const ClipParseResult parsed = parseClip(text);
    if (!parsed.ok()){
        error = "the written file is not a clip: " + parsed.error.toString();
        return false;
    }
    const std::shared_ptr<const AnimationClip> clip = AnimationClip::build(parsed);
    result.measured = measureClipError(*result.source, *clip, rig, result.skinDistanceMm);
    if (!result.measured.measured){
        error = "nothing could be measured on the written file";
        return false;
    }
    result.after = statsOf(clip, editor::FileUtils::pathToGenericUtf8(file));
    return true;
}

nlohmann::json anim::clipOptimizeJson(const ClipOptimizeResult& result){
    nlohmann::json out = clipErrorJson(result.measured);
    out["clip"] = result.displayName;
    out["written_to"] = result.writtenTo.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.writtenTo);
    out["bytes_before"] = result.before.runtimeBytes;
    out["bytes_after"] = result.after.runtimeBytes;
    out["keys_before"] = result.before.keys;
    out["keys_after"] = result.after.keys;
    out["skipped_tracks"] = clipSkippedTracksJson(result.skipped);
    out["kept"] = {{"events", result.eventsKept}};
    out["kept_all_keys"] = result.keptAllKeys;
    return out;
}
