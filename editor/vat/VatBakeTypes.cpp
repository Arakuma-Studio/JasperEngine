// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatBakeTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

using namespace jasperengine;
using namespace jasperengine::vat;
using Json = nlohmann::json;

namespace jasperengine::editor::vat{

VatPresetLimits presetLimits(VatPreset preset){
    VatPresetLimits limits;
    if (preset == VatPreset::Desktop){
        limits.width = 4096;
        limits.maxTexels = 4096ull * 8192ull;
        limits.maxHeight = 8192;
        limits.memoryWarningBytes = 256ull * 1024ull * 1024ull;
    }
    return limits;
}

const char* presetName(VatPreset preset){
    return preset == VatPreset::Desktop ? "desktop" : "mobile_web";
}

bool presetFromName(const std::string& name, VatPreset& out){
    if (name == "mobile_web"){ out = VatPreset::MobileWeb; return true; }
    if (name == "desktop"){ out = VatPreset::Desktop; return true; }
    return false;
}

const char* modeChoiceName(VatModeChoice mode){
    switch (mode){
        case VatModeChoice::Vertices: return "vertices";
        case VatModeChoice::Bones: return "bones";
        default: return "auto";
    }
}

bool modeChoiceFromName(const std::string& name, VatModeChoice& out){
    if (name == "auto"){ out = VatModeChoice::Auto; return true; }
    if (name == "vertices"){ out = VatModeChoice::Vertices; return true; }
    if (name == "bones"){ out = VatModeChoice::Bones; return true; }
    return false;
}

const char* precisionChoiceName(VatPrecisionChoice precision){
    switch (precision){
        case VatPrecisionChoice::Half: return "half";
        case VatPrecisionChoice::Float: return "float";
        default: return "auto";
    }
}

bool precisionChoiceFromName(const std::string& name, VatPrecisionChoice& out){
    if (name == "auto"){ out = VatPrecisionChoice::Auto; return true; }
    if (name == "half"){ out = VatPrecisionChoice::Half; return true; }
    if (name == "float"){ out = VatPrecisionChoice::Float; return true; }
    return false;
}

const char* placeName(VatPlace place){
    switch (place){
        case VatPlace::Beside: return "beside";
        case VatPlace::Replace: return "replace";
        case VatPlace::Crowd: return "crowd";
        default: return "none";
    }
}

bool placeFromName(const std::string& name, VatPlace& out){
    if (name == "none"){ out = VatPlace::None; return true; }
    if (name == "beside"){ out = VatPlace::Beside; return true; }
    if (name == "replace"){ out = VatPlace::Replace; return true; }
    if (name == "crowd"){ out = VatPlace::Crowd; return true; }
    return false;
}

const char* sourceKindName(VatSourceKind kind){
    switch (kind){
        case VatSourceKind::Anim: return "anim";
        case VatSourceKind::Entity: return "entity";
        case VatSourceKind::Pose: return "pose";
        default: return "model";
    }
}

bool sourceKindFromName(const std::string& name, VatSourceKind& out){
    if (name == "model"){ out = VatSourceKind::Model; return true; }
    if (name == "anim"){ out = VatSourceKind::Anim; return true; }
    if (name == "entity"){ out = VatSourceKind::Entity; return true; }
    if (name == "pose"){ out = VatSourceKind::Pose; return true; }
    return false;
}

void clipFrames(float length, ClipWrapMode wrap, float fps, int& frames, float& sampleFps){
    const double l = std::max(0.0, (double)length);
    const double f = fps > 0.0f ? (double)fps : 30.0;
    // a clip shorter than a hundredth of a sample is one pose
    if (l * f < 0.01){
        frames = 1;
        sampleFps = (float)f;
        return;
    }
    const int steps = std::max(1, (int)std::lround(l * f));
    if (wrap == ClipWrapMode::Loop){
        frames = steps;
        sampleFps = (float)((double)steps / l);
    }else{
        frames = steps + 1;
        sampleFps = (float)((double)steps / l);
    }
}

float clipSampleTime(float length, ClipWrapMode wrap, int frames, float sampleFps, int index){
    if (frames <= 1) return 0.0f;
    const int steps = wrap == ClipWrapMode::Loop ? frames : frames - 1;
    // the last sample of a non-loop clip is exactly its length
    if (wrap != ClipWrapMode::Loop && index == steps) return length;
    (void)sampleFps;
    return (float)((double)index * (double)length / (double)steps);
}

uint64_t payloadTexels(VatMode mode, int samples, int elements, bool normals){
    const uint64_t perSample = mode == VatMode::Bones ? 2 : (normals ? 2 : 1);
    return (uint64_t)std::max(samples, 0) * (uint64_t)std::max(elements, 0) * perSample;
}

int payloadHeight(uint64_t texels, int width){
    if (width < 1) return 0;
    return (int)std::max<uint64_t>(1, (texels + (uint64_t)width - 1) / (uint64_t)width);
}

uint64_t payloadBytes(int width, int height, VatPrecision precision){
    return (uint64_t)std::max(width, 0) * (uint64_t)std::max(height, 0) * (precision == VatPrecision::Float ? 16u : 8u);
}

int fetchesPerVertex(VatMode mode, bool normals, bool rigidOnly){
    if (mode == VatMode::Bones) return rigidOnly ? 4 : 16;
    return normals ? 4 : 2;
}

// ---- size model ----

namespace{

    std::string clipNamesJson(const std::vector<VatClipEstimate>& clips, size_t skip){
        Json names = Json::array();
        for (size_t i = 0; i < clips.size(); i++){
            if (i != skip) names.push_back(clips[i].name);
        }
        return names.dump();
    }

}

uint64_t sizeTexels(const VatSizeInputs& inputs){
    int samples = 0;
    for (const VatClipEstimate& clip : inputs.clips){
        const float requested = clip.requestedFps > 0.0f && clip.requestedFps != inputs.fps ? clip.requestedFps : inputs.fps;
        int frames = 1;
        float fps = requested;
        clipFrames(clip.length, clip.wrap, requested, frames, fps);
        samples += frames;
    }
    const int elements = inputs.mode == VatMode::Bones ? inputs.bones : inputs.vertices;
    return payloadTexels(inputs.mode, samples, elements, inputs.normals);
}

VatIssue tooLargeIssue(const VatSizeInputs& inputs){
    const VatPresetLimits limits = presetLimits(inputs.preset);
    const uint64_t texels = sizeTexels(inputs);
    VatIssue issue;
    issue.kind = "too_large";
    issue.error = true;
    char text[256];
    std::snprintf(text, sizeof(text), "the payload needs %llu texels (%d rows of %d), the %s preset holds %llu (%d rows)",
                  (unsigned long long)texels, payloadHeight(texels, limits.width), limits.width, presetName(inputs.preset),
                  (unsigned long long)limits.maxTexels, limits.maxHeight);
    issue.message = text;

    // each candidate fix with the share of its limit it would use
    struct Candidate{
        VatFix fix;
        double share;
    };
    std::vector<Candidate> candidates;
    auto share = [](uint64_t used, uint64_t limit){ return limit ? (double)used / (double)limit : 1e30; };

    // lower fps: the highest whole fps that fits
    if (inputs.fps > 1.0f){
        for (int fps = (int)std::ceil(inputs.fps) - 1; fps >= 1; fps--){
            VatSizeInputs lower = inputs;
            lower.fps = (float)fps;
            for (VatClipEstimate& clip : lower.clips){
                if (clip.requestedFps > (float)fps) clip.requestedFps = (float)fps;
            }
            const uint64_t used = sizeTexels(lower);
            if (used <= limits.maxTexels){
                candidates.push_back({{"Lower to " + std::to_string(fps) + " fps", "fps", std::to_string(fps)}, share(used, limits.maxTexels)});
                break;
            }
        }
    }
    if (inputs.mode == VatMode::Vertices && inputs.normals){
        VatSizeInputs without = inputs;
        without.normals = false;
        candidates.push_back({{"Drop normals (no tangent frames)", "normals", "false"}, share(sizeTexels(without), limits.maxTexels)});
    }
    if (inputs.mode == VatMode::Vertices && inputs.bonesPossible && inputs.bones > 0 && inputs.bones <= VAT_MAX_BONES){
        VatSizeInputs bones = inputs;
        bones.mode = VatMode::Bones;
        candidates.push_back({{"Bake in Bones mode", "mode", "\"bones\""}, share(sizeTexels(bones), limits.maxTexels)});
    }
    if (inputs.clips.size() > 1){
        // the clip with the most frames goes
        size_t largest = 0;
        int largestFrames = -1;
        for (size_t i = 0; i < inputs.clips.size(); i++){
            int frames = 1;
            float fps = 0.0f;
            const VatClipEstimate& clip = inputs.clips[i];
            clipFrames(clip.length, clip.wrap, clip.requestedFps > 0.0f ? clip.requestedFps : inputs.fps, frames, fps);
            if (frames > largestFrames){
                largestFrames = frames;
                largest = i;
            }
        }
        VatSizeInputs fewer = inputs;
        fewer.clips.erase(fewer.clips.begin() + (long)largest);
        candidates.push_back({{"Leave out " + inputs.clips[largest].name, "clips", clipNamesJson(inputs.clips, largest)},
                              share(sizeTexels(fewer), limits.maxTexels)});
    }
    if (inputs.preset == VatPreset::MobileWeb){
        const VatPresetLimits desktop = presetLimits(VatPreset::Desktop);
        VatSizeInputs bigger = inputs;
        bigger.preset = VatPreset::Desktop;
        candidates.push_back({{"Use the desktop preset", "preset", "\"desktop\""}, share(sizeTexels(bigger), desktop.maxTexels)});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b){ return a.share < b.share; });
    std::string fix;
    for (const Candidate& candidate : candidates){
        issue.fixes.push_back(candidate.fix);
        if (!fix.empty()) fix += ", or ";
        fix += candidate.fix.label;
    }
    issue.fix = fix.empty() ? "bake fewer vertices or shorter clips" : fix;
    return issue;
}

// ---- JSON ----

namespace{

    // 3 decimals: reports are read by people and agents, not compared bit for bit
    double rounded(double value, double scale = 1000.0){
        if (!std::isfinite(value)) return 0.0;
        return std::round(value * scale) / scale;
    }

}

Json estimateJson(const VatEstimate& estimate){
    Json clips = Json::array();
    for (const VatClipEstimate& clip : estimate.clips){
        clips.push_back({{"name", clip.name}, {"frames", clip.frames}, {"fps", rounded(clip.fps)},
                         {"length", rounded(clip.length)}, {"bytes", clip.bytes}});
    }
    return {
        {"mode", modeName(estimate.mode)},
        {"mode_reason", estimate.modeReason},
        {"vertices", estimate.vertices},
        {"bones", estimate.bones},
        {"clips", clips},
        {"texture", {{"width", estimate.width}, {"height", estimate.height}, {"format", textureFormatName(estimate.precision)}}},
        {"texels", estimate.texels},
        {"max_texels", estimate.maxTexels},
        {"bytes", estimate.bytes},
        {"fetches_per_vertex", estimate.fetchesPerVertex},
        {"issues", issuesJson(estimate.issues)},
    };
}

// ---- recipe ----

VatRecipe recipeFromSpec(const VatBakeSpec& spec){
    VatRecipe recipe;
    VatRecipeSource source;
    source.kind = sourceKindName(spec.source.kind);
    source.path = spec.source.path;
    source.scene = spec.source.scene;
    switch (spec.source.kind){
        case VatSourceKind::Anim:
        case VatSourceKind::Pose:
            source.clips = spec.source.anims;
            break;
        default:
            source.clips = spec.clips;
            break;
    }
    recipe.sources.push_back(source);
    recipe.mode = modeChoiceName(spec.mode);
    recipe.fps = spec.fps;
    recipe.preset = presetName(spec.preset);
    recipe.normals = spec.normals;
    recipe.precision = precisionChoiceName(spec.precision);
    recipe.toleranceMm = spec.toleranceMm;
    recipe.sockets = spec.sockets;
    return recipe;
}

bool specFromRecipe(const VatRecipe& recipe, VatBakeSpec& spec, std::string& error){
    spec = VatBakeSpec();
    if (recipe.sources.size() != 1){
        error = "the recipe has " + std::to_string(recipe.sources.size()) + " sources: the baker reads one";
        return false;
    }
    const VatRecipeSource& source = recipe.sources[0];
    if (source.kind == "houdini_vat3" || source.kind == "openvat"){
        error = "baked by the " + source.kind + " importer: re-import it (Import as Vertex Animation)";
        return false;
    }
    if (!sourceKindFromName(source.kind, spec.source.kind)){
        error = "unknown recipe source kind \"" + source.kind + "\"";
        return false;
    }
    spec.source.path = source.path;
    spec.source.scene = source.scene;
    if (spec.source.kind == VatSourceKind::Anim || spec.source.kind == VatSourceKind::Pose){
        spec.source.anims = source.clips;
    }else{
        spec.clips = source.clips;
    }
    if (!modeChoiceFromName(recipe.mode, spec.mode)) spec.mode = VatModeChoice::Auto;
    spec.fps = recipe.fps > 0.0f ? recipe.fps : 30.0f;
    if (!presetFromName(recipe.preset, spec.preset)) spec.preset = VatPreset::MobileWeb;
    spec.normals = recipe.normals;
    if (!precisionChoiceFromName(recipe.precision, spec.precision)) spec.precision = VatPrecisionChoice::Auto;
    spec.toleranceMm = recipe.toleranceMm > 0.0f ? recipe.toleranceMm : 1.0f;
    spec.sockets = recipe.sockets;
    return true;
}

bool sameRecipeSource(const VatRecipeSource& a, const VatRecipeSource& b){
    if (a.kind != b.kind || a.path != b.path || a.scene != b.scene) return false;
    std::set<std::string> left(a.clips.begin(), a.clips.end());
    std::set<std::string> right(b.clips.begin(), b.clips.end());
    return left == right;
}

}
