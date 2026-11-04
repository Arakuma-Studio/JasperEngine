// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipReport.h"

#include "animation/ModelSubClips.h"
#include "io/FileData.h"
#include "pool/AnimationClipPool.h"
#include "util/FileUtils.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

using namespace jasperengine;

namespace{

    bool same(float a, float b){ return a == b; }
    bool same(const Vector3& a, const Vector3& b){ return a.x == b.x && a.y == b.y && a.z == b.z; }
    bool same(const Quaternion& a, const Quaternion& b){ return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z; }

    bool isZero(float value){ return value == 0.0f; }
    bool isZero(const Vector3& value){ return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f; }
    bool isZero(const Quaternion& value){ return value.w == 0.0f && value.x == 0.0f && value.y == 0.0f && value.z == 0.0f; }

    // every row of `width` values equals the first; tangents, when there are any, are all zero
    template<class T>
    bool holdsStill(const T* values, const T* in, const T* out, size_t keys, size_t width){
        for (size_t i = 0; i < keys * width; i++){
            if (!same(values[i], values[i % width])) return false;
            if (in && !(isZero(in[i]) && isZero(out[i]))) return false;
        }
        return true;
    }

    // Exact: near-constant is the reducer's decision, with a measured error, not a fact to report
    bool isConstant(const AnimationClip::Track& track){
        if (track.keyCount < 2) return false;
        if (track.rotations) return holdsStill(track.rotations, track.inRotations, track.outRotations, track.keyCount, track.width);
        if (track.vectors) return holdsStill(track.vectors, track.inVectors, track.outVectors, track.keyCount, track.width);
        if (track.weights) return holdsStill(track.weights, track.inWeights, track.outWeights, track.keyCount, track.width);
        return false;
    }

    std::optional<uint64_t> fileSizeOf(const std::string& reference){
        if (reference.empty() || anim::isSubClipReference(reference)) return std::nullopt;
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(editor::FileUtils::pathFromUtf8(FileData::getSystemPath(reference)), error);
        if (error) return std::nullopt;
        return (uint64_t)size;
    }

    std::string withThousands(uint64_t value){
        std::string digits = std::to_string(value);
        for (size_t at = digits.size(); at > 3; at -= 3) digits.insert(at - 3, ",");
        return digits;
    }

}

anim::ClipReport anim::analyzeClip(const ClipReportRequest& request){
    ClipReport report;
    std::shared_ptr<const AnimationClip> clip = request.clip;
    bool loaded = false;
    if (!clip){
        if (request.reference.empty()){
            report.error = "no clip";
            return report;
        }
        clip = AnimationClipPool::get(request.reference);
        if (!clip){
            clip = AnimationClipPool::getOrLoad(request.reference, &report.error);
            loaded = clip != nullptr;
        }
        if (!clip) return report;
    }

    report.ok = true;
    report.runtimeBytes = clip->memoryBytes();
    report.fileBytes = fileSizeOf(request.reference);
    report.tracks = clip->getTracks().size();
    report.lengthSeconds = clip->getLength();
    for (const AnimationClip::Track& track : clip->getTracks()){
        report.keys += track.keyCount;
        if (isConstant(track)) report.constantTracks++;
    }

    if (loaded){
        clip.reset();
        AnimationClipPool::releaseUnused(request.reference);
    }
    return report;
}

const anim::ClipReportField* anim::findClipReportField(std::string_view key){
    for (const ClipReportField& field : kClipReportFields){
        if (key == field.key) return &field;
    }
    return nullptr;
}

std::string anim::formatClipField(const ClipReportField& field, double value){
    char text[32];
    switch (field.format){
        case ClipFieldFormat::Bytes:
            if (value < 1024.0) return withThousands((uint64_t)value) + " B";
            if (value < 1024.0 * 1024.0){
                std::snprintf(text, sizeof(text), "%.0f KB", value / 1024.0);
            }else{
                std::snprintf(text, sizeof(text), "%.1f MB", value / (1024.0 * 1024.0));
            }
            return text;
        case ClipFieldFormat::Count:
            return withThousands((uint64_t)value);
        case ClipFieldFormat::Seconds:
            std::snprintf(text, sizeof(text), "%.2f s", value);
            return text;
    }
    return std::string();
}

nlohmann::json anim::clipReportJson(const ClipReport& report){
    nlohmann::json out = nlohmann::json::object();
    out["runtime_bytes"] = report.runtimeBytes;
    out["file_bytes"] = report.fileBytes ? nlohmann::json(*report.fileBytes) : nlohmann::json(nullptr);
    out["keys"] = report.keys;
    out["tracks"] = report.tracks;
    // the float's own digits, not its expansion as a double (1.33, not 1.3300000429)
    out["length_s"] = std::round((double)report.lengthSeconds * 1000000.0) / 1000000.0;
    out["constant_tracks"] = report.constantTracks;
    return out;
}
