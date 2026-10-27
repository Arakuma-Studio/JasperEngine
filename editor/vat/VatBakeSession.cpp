// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatBakeSession.h"

#include "Project.h"
#include "animation/ClipConverters.h"
#include "animation/ClipEditorUtil.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateVatCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "physics/PlacementSession.h"
#include "pose/PoseEditSession.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"

#include "Scene.h"
#include "component/Transform.h"
#include "pool/VertexAnimationPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::vat;

namespace jasperengine::editor::vat{

namespace{

    VatIssue makeIssue(const std::string& kind, bool error, const std::string& message, const std::string& fix){
        VatIssue issue;
        issue.kind = kind;
        issue.error = error;
        issue.message = message;
        issue.fix = fix;
        return issue;
    }

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    bool endsWith(const std::string& text, const std::string& suffix){
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // a file name without the characters file systems refuse
    std::string fileSafe(const std::string& name){
        std::string out;
        for (char c : name){
            out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
        }
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
        return out.empty() ? std::string("vertex animation") : out;
    }

    bool fileExists(const fs::path& path){
        std::error_code ec;
        return fs::exists(path, ec);
    }

    std::string joinNames(const std::vector<std::string>& names, size_t max){
        std::string out;
        for (size_t i = 0; i < names.size() && i < max; i++) out += (i ? ", " : "") + names[i];
        if (names.size() > max) out += ", ...";
        return out;
    }

    // a .vat file's data, nullopt when it can't be read
    bool readVatFile(const fs::path& file, VatData& out, std::string* error){
        std::string text;
        if (!clipui::readWholeFile(file, text)){
            if (error) *error = "can't read " + FileUtils::pathToGenericUtf8(file);
            return false;
        }
        VatParseResult parsed = parseVat(text);
        if (!parsed.ok()){
            if (error) *error = parsed.error;
            return false;
        }
        out = *parsed.data;
        return true;
    }

    std::atomic<uint64_t> previewCounter{0};

}

const VatPayload& VatBakeResult::payload() const{
    static const VatPayload empty;
    return preview ? preview->getPayload() : empty;
}

// ---- clip matching ----

std::vector<int> matchClips(const std::vector<VatClip>& oldClips, const std::vector<std::string>& newNames,
                            const std::vector<float>& newLengths){
    std::vector<int> result(newNames.size(), -1);
    std::vector<bool> oldUsed(oldClips.size(), false);
    // by name
    for (size_t n = 0; n < newNames.size(); n++){
        for (size_t o = 0; o < oldClips.size(); o++){
            if (!oldUsed[o] && oldClips[o].name == newNames[n]){
                result[n] = (int)o;
                oldUsed[o] = true;
                break;
            }
        }
    }
    // the renamed ones, in order, preferring a new clip of the same length
    for (size_t o = 0; o < oldClips.size(); o++){
        if (oldUsed[o]) continue;
        int pick = -1;
        for (size_t n = 0; n < newNames.size() && pick < 0; n++){
            if (result[n] >= 0) continue;
            const float length = n < newLengths.size() ? newLengths[n] : -1.0f;
            if (std::fabs(oldClips[o].length() - length) <= 1e-3f * std::max(1.0f, length)) pick = (int)n;
        }
        for (size_t n = 0; n < newNames.size() && pick < 0; n++){
            if (result[n] < 0) pick = (int)n;
        }
        if (pick < 0) break;
        result[(size_t)pick] = (int)o;
        oldUsed[o] = true;
    }
    return result;
}

// ---- session ----

VatBakeSession::VatBakeSession(Project* project): project(project){
}

VatBakeSession::~VatBakeSession(){
    cancel();
    releasePreview();
}

void VatBakeSession::releasePreview(){
    if (bakeResult.previewKey.empty()) return;
    VertexAnimationPool::invalidate(bakeResult.previewKey);
    std::error_code ec;
    fs::remove_all(FileUtils::pathFromUtf8(bakeResult.previewKey).parent_path(), ec);
    bakeResult.previewKey.clear();
    bakeResult.preview.reset();
}

void VatBakeSession::reset(){
    cancel();
    releasePreview();
    poser.reset();
    target.reset();
    checkIssues.clear();
    checked = false;
    selected.clear();
    bakeState = VatBakeState::Idle;
    errorText.clear();
    failure.clear();
    input = VatBakeInput();
    plan.clear();
    planDone = 0;
    output = VatBakeOutput();
    bakeResult = VatBakeResult();
    cancelFlag = false;
    workerDone = false;
    workerProgress = 0.0f;
}

void VatBakeSession::setSpec(const VatBakeSpec& spec){
    reset();
    bakeSpec = spec;
    rebakePath.clear();
    rebakeLoaded = false;
    rebakeOld = VatData();
    rebakeClipMap.clear();
}

bool VatBakeSession::loadRebake(const std::string& vatPath, std::string& error){
    if (!project){
        error = "no project is open";
        return false;
    }
    fs::path given = FileUtils::pathFromUtf8(vatPath);
    const fs::path absolute = (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
    VatData old;
    if (!readVatFile(absolute, old, &error)) return false;
    VatBakeSpec spec;
    if (!specFromRecipe(old.recipe, spec, error)) return false;
    spec.output = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
    spec.overwrite = true;
    setSpec(spec);
    rebakePath = spec.output;
    rebakeOld = old;
    rebakeLoaded = true;

    // the source's clips now, matched to the file's
    check();
    if (!target){
        error = issuesText(checkIssues, true);
        return false;
    }
    std::vector<std::string> names;
    std::vector<float> lengths;
    for (int index : selected){
        names.push_back(target->clips[(size_t)index].name);
        lengths.push_back(target->clips[(size_t)index].length);
    }
    const std::vector<int> match = matchClips(old.clips, names, lengths);
    // the file's clip order, its wraps, its per-clip rates where they differ from the recipe's
    std::vector<std::pair<int, size_t>> order;
    for (size_t n = 0; n < names.size(); n++){
        if (match[n] >= 0) order.push_back({match[n], n});
    }
    std::sort(order.begin(), order.end());
    bakeSpec.clips.clear();
    bakeSpec.clipOverrides.clear();
    for (const auto& entry : order){
        const VatClip& clip = old.clips[(size_t)entry.first];
        const std::string& name = names[entry.second];
        bakeSpec.clips.push_back(name);
        VatClipOverride override;
        override.name = name;
        override.wrap = clip.wrap;
        int frames = 1;
        float fps = 0.0f;
        clipFrames(lengths[entry.second], clip.wrap, bakeSpec.fps, frames, fps);
        if (std::fabs(fps - clip.fps) > 1e-4f * std::max(1.0f, clip.fps)) override.fps = clip.fps;
        bakeSpec.clipOverrides.push_back(override);
        rebakeClipMap.push_back({name, entry.first});
    }
    if (bakeSpec.clips.empty()){
        error = "none of the file's clips is in its source any more";
        return false;
    }
    // check again with the clip list and overrides (the source stays resolved)
    checked = false;
    checkIssues.clear();
    return true;
}

const std::vector<VatSourceClip>& VatBakeSession::sourceClips() const{
    static const std::vector<VatSourceClip> none;
    return target ? target->clips : none;
}

namespace{

    const VatClipOverride* overrideFor(const VatBakeSpec& spec, const std::string& name){
        for (const VatClipOverride& override : spec.clipOverrides){
            if (override.name == name) return &override;
        }
        return nullptr;
    }

    ClipWrapMode effectiveWrap(const VatBakeSpec& spec, const VatSourceClip& clip){
        const VatClipOverride* override = overrideFor(spec, clip.name);
        return override && override->wrap != ClipWrapMode::Default ? override->wrap : clip.wrap;
    }

    float effectiveFps(const VatBakeSpec& spec, const VatSourceClip& clip){
        const VatClipOverride* override = overrideFor(spec, clip.name);
        return override && override->fps > 0.0f ? override->fps : spec.fps;
    }

    bool clipMovesShape(const VatSourceClip& clip){
        for (const AnimationClip::Track& track : clip.clip->getTracks()){
            if ((track.channel == ClipChannel::Morph || track.channel == ClipChannel::Lattice) && track.isSampled() &&
                std::find(clip.unbound.begin(), clip.unbound.end(), track.path) == clip.unbound.end()){
                return true;
            }
        }
        return false;
    }

}

const std::vector<VatIssue>& VatBakeSession::check(){
    if (checked) return checkIssues;
    checked = true;
    checkIssues.clear();
    const VatBakeSpec& spec = bakeSpec;

    // settings
    if (!(spec.fps > 0.0f) || spec.fps > 240.0f){
        checkIssues.push_back(makeIssue("invalid_settings", true, "fps must be over 0 and at most 240", "use 30 fps"));
    }
    if (!(spec.toleranceMm > 0.0f)){
        checkIssues.push_back(makeIssue("invalid_settings", true, "tolerance_mm must be over 0", "use 1 mm"));
    }
    for (const VatClipOverride& override : spec.clipOverrides){
        if (override.fps < 0.0f || override.fps > 240.0f){
            checkIssues.push_back(makeIssue("invalid_settings", true, "the fps of clip '" + override.name + "' must be 0 (the bake's) to 240",
                                            "use the bake's fps"));
        }
    }
    if (!project){
        checkIssues.push_back(makeIssue("invalid_source", true, "no project is open", "open a project"));
        return checkIssues;
    }
    if (!spec.output.empty()){
        const std::string extension = lower(spec.output);
        fs::path given = FileUtils::pathFromUtf8(spec.output);
        const fs::path absolute = (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
        if (!endsWith(extension, ".vat")){
            checkIssues.push_back(makeIssue("invalid_settings", true, "the output '" + spec.output + "' is not a .vat file",
                                            "name it <name>.vat (the mesh goes to <name>.vat.glb)"));
        }else if (!project->isInsideAssetsPath(absolute)){
            checkIssues.push_back(makeIssue("invalid_settings", true, "the output '" + spec.output + "' is outside the assets folder",
                                            "write it under assets/"));
        }else if (!spec.overwrite && (fileExists(absolute) || fileExists(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute)))))){
            VatIssue issue = makeIssue("output_exists", true, "'" + spec.output + "' exists", "pass overwrite, or choose another output");
            issue.fixes.push_back({"Overwrite it", "overwrite", "true"});
            checkIssues.push_back(issue);
        }
    }
    if (spec.place == VatPlace::Crowd){
        checkIssues.push_back(makeIssue("invalid_settings", true, "crowd placement is not available yet",
                                        "bake with place beside, then turn the GameObject into a crowd"));
    }
    if (spec.place == VatPlace::Replace && !isGameObjectSource(spec.source)){
        checkIssues.push_back(makeIssue("invalid_settings", true, "place replace needs a GameObject source (a file has nothing to replace)",
                                        "use place beside"));
    }

    // the source
    if (!target){
        target = std::make_unique<VatBakeTarget>();
        if (!resolveVatSource(project, spec.source, *target, checkIssues)){
            target.reset();
            return checkIssues;
        }
    }

    // the clips
    selected.clear();
    if (spec.clips.empty()){
        for (size_t i = 0; i < target->clips.size(); i++) selected.push_back((int)i);
    }else{
        std::vector<std::string> available;
        for (const VatSourceClip& clip : target->clips) available.push_back(clip.name);
        for (const std::string& name : spec.clips){
            const int index = target->findClip(name);
            if (index < 0){
                checkIssues.push_back(makeIssue("invalid_settings", true, "the source has no clip '" + name + "'",
                                                "choose from: " + joinNames(available, 12)));
            }else if (std::find(selected.begin(), selected.end(), index) == selected.end()){
                selected.push_back(index);
            }
        }
    }
    if (selected.size() > (size_t)VAT_MAX_CLIPS){
        checkIssues.push_back(makeIssue("clip_limit", true, std::to_string(selected.size()) + " clips, a Vertex Animation holds at most " +
                                        std::to_string(VAT_MAX_CLIPS), "bake the clips in two assets"));
    }
    bool anyBound = false;
    for (int index : selected){
        const VatSourceClip& clip = target->clips[(size_t)index];
        if (clip.tracks > (int)clip.unbound.size()) anyBound = true;
        if (!clip.unbound.empty()){
            checkIssues.push_back(makeIssue("unbound_tracks", false,
                                            std::to_string(clip.unbound.size()) + " track(s) of '" + clip.name + "' animate nothing under the source (" +
                                            joinNames(clip.unbound, 3) + ")",
                                            "rename the nodes or fix the clip's paths in the Animation window (Fix Paths)"));
        }
    }
    if (!selected.empty() && !anyBound){
        checkIssues.push_back(makeIssue("no_animation", true, "no track of the chosen clips animates anything under the source",
                                        "play the clips on the model they were made for"));
    }
    if (hasErrors(checkIssues)) return checkIssues;

    // the renderers and sockets
    poser = std::make_unique<VatPoser>();
    std::string error;
    if (!poser->begin(*target, spec.sockets, checkIssues, error)){
        if (!error.empty()) checkIssues.push_back(makeIssue("invalid_source", true, error, "check the source's meshes"));
        poser.reset();
        return checkIssues;
    }
    for (const VatIssue& warning : poser->warnings) checkIssues.push_back(warning);

    // the sizes
    const VatEstimate sizes = estimate();
    for (const VatIssue& issue : sizes.issues) checkIssues.push_back(issue);
    return checkIssues;
}

VatEstimate VatBakeSession::estimate(){
    VatEstimate out;
    if (!checked) check();
    if (!target || !poser) return out;
    const VatBakeSpec& spec = bakeSpec;
    const VatPresetLimits limits = presetLimits(spec.preset);

    out.vertices = poser->estimatedVertices();
    std::vector<const VatSourceClip*> clips;
    for (int index : selected) clips.push_back(&target->clips[(size_t)index]);
    out.bones = poser->estimatedBones(poser->animatedEntities(clips));
    out.renderers = (int)poser->renderers.size();

    bool shapes = false;
    for (int index : selected) shapes = shapes || clipMovesShape(target->clips[(size_t)index]);
    bool latticeAtRest = false;
    for (const VatBakeRenderer& renderer : poser->renderers){
        for (const cpuskin::CpuSkinLatticeBinding& binding : renderer.restLattice.bindings) latticeAtRest = latticeAtRest || binding.moving;
    }
    const bool bonesPossible = !shapes && !latticeAtRest && out.bones <= VAT_MAX_BONES;
    if (spec.mode == VatModeChoice::Vertices){
        out.mode = VatMode::Vertices;
        out.modeReason = "Vertices mode was chosen";
    }else if (spec.mode == VatModeChoice::Bones){
        out.mode = VatMode::Bones;
        out.modeReason = "Bones mode was chosen";
    }else if (out.bones > VAT_MAX_BONES){
        out.mode = VatMode::Vertices;
        out.modeReason = "likely Vertices: about " + std::to_string(out.bones) + " bones, over the " + std::to_string(VAT_MAX_BONES) +
                         " a Bones bake holds";
    }else if (shapes || latticeAtRest){
        out.mode = VatMode::Vertices;
        out.modeReason = shapes ? "likely Vertices: the clips animate morph targets or lattices"
                                : "likely Vertices: a lattice deforms the mesh";
    }else{
        out.mode = VatMode::Bones;
        out.modeReason = "likely Bones: skin and rigid parts only (the bake measures it)";
    }
    out.precision = out.mode == VatMode::Bones || spec.precision == VatPrecisionChoice::Float ? VatPrecision::Float : VatPrecision::Half;

    VatSizeInputs size;
    size.mode = out.mode;
    size.vertices = out.vertices;
    size.bones = out.bones;
    size.normals = spec.normals;
    size.bonesPossible = bonesPossible;
    size.preset = spec.preset;
    size.fps = spec.fps;
    const int elements = out.mode == VatMode::Bones ? out.bones : out.vertices;
    for (int index : selected){
        const VatSourceClip& source = target->clips[(size_t)index];
        VatClipEstimate clip;
        clip.name = source.name;
        clip.length = source.length;
        clip.wrap = effectiveWrap(spec, source);
        clip.requestedFps = effectiveFps(spec, source);
        clipFrames(clip.length, clip.wrap, clip.requestedFps, clip.frames, clip.fps);
        clip.bytes = payloadTexels(out.mode, clip.frames, elements, spec.normals) * (out.precision == VatPrecision::Float ? 16u : 8u);
        out.samples += clip.frames;
        out.clips.push_back(clip);
        size.clips.push_back(clip);
    }
    out.texels = sizeTexels(size);
    out.maxTexels = limits.maxTexels;
    out.width = limits.width;
    out.height = payloadHeight(out.texels, limits.width);
    out.bytes = payloadBytes(out.width, out.height, out.precision);
    bool rigidOnly = true;
    for (const VatBakeRenderer& renderer : poser->renderers){
        for (const cpuskin::CpuSkinSubmesh& submesh : renderer.source.submeshes) rigidOnly = rigidOnly && !submesh.hasSkinning;
    }
    out.fetchesPerVertex = fetchesPerVertex(out.mode, spec.normals, rigidOnly);

    if (out.vertices > VAT_MAX_VERTICES){
        out.issues.push_back(makeIssue("vertex_limit", true, std::to_string(out.vertices) + " vertices, a Vertex Animation holds at most " +
                                       std::to_string(VAT_MAX_VERTICES), "bake a simpler mesh (a LOD of the model)"));
    }
    if (spec.mode == VatModeChoice::Bones && out.bones > VAT_MAX_BONES){
        VatIssue issue = makeIssue("bone_limit", true, std::to_string(out.bones) + " bones, a Bones bake holds at most " +
                                   std::to_string(VAT_MAX_BONES), "bake in Vertices mode");
        issue.fixes.push_back({"Bake in Vertices mode", "mode", "\"vertices\""});
        out.issues.push_back(issue);
    }else if (spec.mode == VatModeChoice::Bones && (shapes || latticeAtRest)){
        out.issues.push_back(makeIssue("bone_limit", false, "the clips move morph targets or lattices, which bones can't reproduce",
                                       "bake in Vertices mode (or Auto)"));
    }
    if (out.texels > limits.maxTexels || out.height > limits.maxHeight){
        out.issues.push_back(tooLargeIssue(size));
    }else if (out.bytes > limits.memoryWarningBytes){
        out.issues.push_back(makeIssue("memory_high", false, "the payload would take about " + std::to_string(out.bytes / (1024 * 1024)) +
                                       " MB, over the " + std::to_string(limits.memoryWarningBytes / (1024 * 1024)) + " MB the " +
                                       presetName(spec.preset) + " preset suggests",
                                       "lower fps, drop normals, leave out clips or use Bones mode"));
    }
    return out;
}

void VatBakeSession::finishFailed(const std::string& message, bool cancelled){
    if (poser) poser->restore();
    bakeState = cancelled ? VatBakeState::Cancelled : VatBakeState::Failed;
    errorText = message;
}

bool VatBakeSession::start(){
    if (bakeState == VatBakeState::Posing || bakeState == VatBakeState::Encoding) return false;
    if (bakeState != VatBakeState::Idle){
        // a second run of the same spec: resolve again (the scene may have changed)
        const VatBakeSpec spec = bakeSpec;
        const std::string rebake = rebakePath;
        const bool loaded = rebakeLoaded;
        const VatData old = rebakeOld;
        const auto map = rebakeClipMap;
        setSpec(spec);
        rebakePath = rebake;
        rebakeLoaded = loaded;
        rebakeOld = old;
        rebakeClipMap = map;
    }
    check();
    failure.clear();
    if (hasErrors(checkIssues) || !target || !poser){
        for (const VatIssue& issue : checkIssues){
            if (issue.error) failure.push_back(issue);
        }
        finishFailed(issuesText(checkIssues, true), false);
        return false;
    }
    if (!target->scratch()){
        SceneProject* sceneProject = project->getScene(target->sceneId);
        if (!sceneProject || sceneProject->playState != ScenePlayState::STOPPED){
            failure.push_back(makeIssue("scene_playing", true, "the scene is playing or closed", "stop play mode before baking"));
            finishFailed(failure.back().message, false);
            return false;
        }
        PoseEditSession::suspendActiveFor(target->sceneId, {target->root});
        sceneStamp = placementSceneEditStamp(project, target->sceneId);
    }
    buildPlan();
    cancelFlag = false;
    bakeState = VatBakeState::Posing;
    return true;
}

void VatBakeSession::buildPlan(){
    input = VatBakeInput();
    plan.clear();
    planDone = 0;
    const VatBakeSpec& spec = bakeSpec;
    input.mode = spec.mode;
    input.precision = spec.precision;
    input.normals = spec.normals;
    input.toleranceMm = spec.toleranceMm;
    input.preset = spec.preset;
    input.socketPaths = spec.sockets;
    for (int index : selected){
        const VatSourceClip& source = target->clips[(size_t)index];
        VatBakeClipInput clip;
        clip.name = source.name;
        clip.wrap = effectiveWrap(spec, source);
        clip.length = source.length;
        clip.events = source.events;
        clipFrames(clip.length, clip.wrap, effectiveFps(spec, source), clip.frames, clip.fps);
        clip.samples.resize((size_t)clip.frames);
        clip.midFrames = midFramesFor(clip.frames, clip.wrap);
        clip.mids.resize(clip.midFrames.size());
        const int clipIndex = (int)input.clips.size();
        for (int f = 0; f < clip.frames; f++){
            plan.push_back({clipIndex, false, f, clipSampleTime(clip.length, clip.wrap, clip.frames, clip.fps, f)});
        }
        for (size_t m = 0; m < clip.midFrames.size(); m++){
            const int f = clip.midFrames[m];
            const float t0 = clipSampleTime(clip.length, clip.wrap, clip.frames, clip.fps, f);
            const float t1 = f + 1 >= clip.frames ? clip.length : clipSampleTime(clip.length, clip.wrap, clip.frames, clip.fps, f + 1);
            plan.push_back({clipIndex, true, (int)m, (t0 + t1) * 0.5f});
        }
        input.clips.push_back(std::move(clip));
    }
}

bool VatBakeSession::stampChanged(){
    if (!target || target->scratch()) return false;
    return placementSceneEditStamp(project, target->sceneId) != sceneStamp;
}

bool VatBakeSession::advance(double budgetMs){
    if (bakeState == VatBakeState::Posing){
        if (cancelFlag){
            finishFailed("cancelled", true);
            return true;
        }
        if (!target->scratch()){
            SceneProject* sceneProject = project->getScene(target->sceneId);
            if (!sceneProject || !sceneProject->scene || sceneProject->playState != ScenePlayState::STOPPED){
                failure.push_back(makeIssue("scene_playing", true, "the scene was closed or started playing during the bake", "bake again"));
                finishFailed(failure.back().message, false);
                return true;
            }
            PoseEditSession::suspendActiveFor(target->sceneId, {target->root});
            if (stampChanged()){
                failure.push_back(makeIssue("invalid_source", true, "the scene changed during the bake", "bake again"));
                finishFailed("the scene changed during the bake: bake again", false);
                return true;
            }
        }
        const auto begin = std::chrono::steady_clock::now();
        while (planDone < plan.size()){
            const PlanItem& item = plan[planDone];
            VatBakeClipInput& clip = input.clips[(size_t)item.clip];
            const VatSourceClip& source = target->clips[(size_t)selected[(size_t)item.clip]];
            VatSampleState& sample = item.mid ? clip.mids[(size_t)item.index] : clip.samples[(size_t)item.index];
            std::string error;
            if (!poser->capture(source, item.time, sample, error)){
                failure.push_back(makeIssue("invalid_source", true, error, "check the source"));
                finishFailed(error, false);
                return true;
            }
            planDone++;
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (budgetMs > 0.0 && elapsed >= budgetMs) break;
        }
        poser->restore();
        if (planDone < plan.size()) return false;
        startWorker();
        if (budgetMs > 0.0) return false;
    }
    if (bakeState == VatBakeState::Encoding){
        if (cancelFlag && budgetMs > 0.0 && !workerDone) return false;
        if (!workerDone && budgetMs > 0.0) return false;
        if (worker.joinable()) worker.join();
        finalize();
        return true;
    }
    return bakeState == VatBakeState::Done || bakeState == VatBakeState::Failed || bakeState == VatBakeState::Cancelled;
}

bool VatBakeSession::run(){
    if (!start()) return false;
    while (!advance(0.0)){}
    return bakeState == VatBakeState::Done;
}

void VatBakeSession::cancel(){
    cancelFlag = true;
    if (bakeState == VatBakeState::Posing){
        finishFailed("cancelled", true);
    }else if (bakeState == VatBakeState::Encoding){
        if (worker.joinable()) worker.join();
        bakeState = VatBakeState::Cancelled;
        errorText = "cancelled";
    }else if (worker.joinable()){
        worker.join();
    }
}

void VatBakeSession::startWorker(){
    // the renderers and materials move into the input (the poser keeps the entities)
    input.renderers = poser->renderers;
    input.materials = poser->materials;
    workerDone = false;
    workerProgress = 0.0f;
    bakeState = VatBakeState::Encoding;
    worker = std::thread([this](){
        std::string error;
        bool ok = false;
        try{
            ok = computeVatBake(input, output, error, &cancelFlag, [this](float fraction){ workerProgress = fraction; });
        }catch (const std::bad_alloc&){
            error = "out of memory while encoding: lower fps or use Bones mode";
        }catch (const std::exception& exception){
            error = exception.what();
        }
        workerOk = ok;
        workerError = error;
        workerDone = true;
    });
}

VatBakeProgress VatBakeSession::progress() const{
    VatBakeProgress out;
    switch (bakeState){
        case VatBakeState::Idle:
            out.stage = "Ready";
            break;
        case VatBakeState::Posing:{
            const size_t index = std::min(planDone, plan.empty() ? 0 : plan.size() - 1);
            const int clip = plan.empty() ? 0 : plan[index].clip;
            out.stage = "Posing clip " + std::to_string(clip + 1) + " of " + std::to_string(input.clips.size()) + ": " +
                        (input.clips.empty() ? std::string() : input.clips[(size_t)clip].name);
            out.fraction = plan.empty() ? 0.0f : 0.5f * (float)planDone / (float)plan.size();
            break;
        }
        case VatBakeState::Encoding:
            out.stage = "Encoding";
            out.fraction = 0.5f + 0.5f * workerProgress.load();
            break;
        case VatBakeState::Done:
            out.stage = "Done";
            out.fraction = 1.0f;
            break;
        case VatBakeState::Failed:
            out.stage = "Failed: " + errorText;
            break;
        case VatBakeState::Cancelled:
            out.stage = "Cancelled";
            break;
    }
    return out;
}

void VatBakeSession::finalize(){
    input = VatBakeInput();     // the samples are no longer needed
    if (!workerOk){
        for (const VatIssue& issue : output.report.warnings){
            if (issue.error) failure.push_back(issue);
        }
        bakeState = cancelFlag || workerError == "cancelled" ? VatBakeState::Cancelled : VatBakeState::Failed;
        errorText = workerError;
        return;
    }
    VatBakeResult& result = bakeResult;
    result.data = output.data;
    result.data.recipe = recipeFromSpec(bakeSpec);
    VatRecipeSource& source = result.data.recipe.sources[0];
    source.clips.clear();
    for (int index : selected){
        const VatSourceClip& clip = target->clips[(size_t)index];
        result.sourceClips.push_back(clip.name);
        if (bakeSpec.source.kind == VatSourceKind::Anim || bakeSpec.source.kind == VatSourceKind::Pose){
            if (std::find(source.clips.begin(), source.clips.end(), clip.origin) == source.clips.end()) source.clips.push_back(clip.origin);
        }else{
            source.clips.push_back(clip.name);
        }
    }
    source.hashes = target->hashes;
    if (isGameObjectSource(bakeSpec.source)) source.scene = target->sceneFile;
    result.mesh = std::move(output.mesh);
    result.sockets = output.socketTracks;
    result.report = output.report;
    result.boneNames = output.boneNames;
    result.embeddedImages = poser->embeddedImages;
    std::vector<VatIssue> warnings;
    for (const VatIssue& issue : checkIssues){
        if (!issue.error && issue.kind != "memory_high") warnings.push_back(issue);
    }
    for (const VatIssue& issue : output.report.warnings) warnings.push_back(issue);
    result.report.warnings = warnings;
    result.data.mesh = fileSafe(target->stem) + VAT_MESH_SUFFIX;
    result.data.layout.layoutHash = computeLayoutHash(result.data);

    std::string error;
    result.preview = VertexAnimation::create(result.data, std::move(output.payload), result.sockets, &error);
    if (!result.preview){
        bakeState = VatBakeState::Failed;
        errorText = "the baked asset is inconsistent: " + error;
        return;
    }
    previewId = ++previewCounter;
    result.previewKey = FileUtils::pathToUtf8((project->getProjectPath() / ".jasperengine" / "cache" / "vat-preview" /
                                               std::to_string(previewId) / FileUtils::pathFromUtf8(fileSafe(target->stem) + VAT_EXTENSION))
                                                  .lexically_normal());
    VertexAnimationPool::put(result.previewKey, result.preview);
    output = VatBakeOutput();
    bakeState = VatBakeState::Done;
}

// ---- output ----

namespace{

    // the mesh with every embedded texture placeholder replaced by `paths`
    void resolveEmbedded(VatMeshData& mesh, const std::vector<std::string>& paths){
        for (VatMaterialData& material : mesh.materials){
            for (std::string* slot : {&material.baseColorTexture, &material.normalTexture, &material.metallicRoughnessTexture,
                                      &material.occlusionTexture, &material.emissiveTexture}){
                if (slot->compare(0, std::char_traits<char>::length(VAT_EMBEDDED_TEXTURE), VAT_EMBEDDED_TEXTURE) != 0) continue;
                const size_t index = (size_t)std::atoi(slot->c_str() + std::char_traits<char>::length(VAT_EMBEDDED_TEXTURE));
                *slot = index < paths.size() ? paths[index] : std::string();
            }
        }
    }

}

bool VatBakeSession::writePreviewFiles(std::string& vatPath, std::string& meshPath, std::string& error){
    if (bakeState != VatBakeState::Done){
        error = "nothing baked yet";
        return false;
    }
    const fs::path vatFile = FileUtils::pathFromUtf8(bakeResult.previewKey);
    const fs::path dir = vatFile.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<std::string> images;
    const std::string stem = FileUtils::pathToUtf8(vatFile.stem());
    for (const VatEmbeddedImage& image : bakeResult.embeddedImages){
        const fs::path file = dir / FileUtils::pathFromUtf8(stem + "_" + image.name + image.extension);
        if (!FileUtils::writeFileReplacing(file, image.bytes)){
            error = "can't write " + FileUtils::pathToGenericUtf8(file);
            return false;
        }
        images.push_back(FileUtils::pathToUtf8(file));
    }
    VatMeshData mesh = bakeResult.mesh;
    resolveEmbedded(mesh, images);
    VatData data = bakeResult.data;
    data.guid = AssetDatabase::generateGuid();
    VatEncodedFiles encoded;
    if (!encodeVatFiles(FileUtils::pathToUtf8(vatFile), data, mesh, bakeResult.payload(), bakeResult.sockets, encoded, &error)) return false;
    if (!FileUtils::writeFileReplacing(vatFile, encoded.vatText) ||
        !FileUtils::writeFileReplacing(FileUtils::pathFromUtf8(encoded.meshPath),
                                       std::string(encoded.glbBytes.begin(), encoded.glbBytes.end()))){
        error = "can't write the preview files under " + FileUtils::pathToGenericUtf8(dir);
        return false;
    }
    vatPath = encoded.vatPath;
    meshPath = encoded.meshPath;
    return true;
}

bool VatBakeSession::plannedOutput(std::string& vatPath, bool& rewrite, std::string& error){
    rewrite = false;
    if (bakeState != VatBakeState::Done){
        error = "nothing baked yet";
        return false;
    }
    const VatRecipeSource& ours = bakeResult.data.recipe.sources[0];
    if (!bakeSpec.output.empty()){
        fs::path given = FileUtils::pathFromUtf8(bakeSpec.output);
        const fs::path absolute = (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
        vatPath = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
        if (!endsWith(lower(vatPath), ".vat") || !project->isInsideAssetsPath(absolute)){
            error = "'" + vatPath + "' is not a .vat path inside the assets folder";
            return false;
        }
        if (fileExists(absolute) || fileExists(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))))){
            if (!bakeSpec.overwrite){
                error = "'" + vatPath + "' exists: pass overwrite, or choose another output";
                return false;
            }
            rewrite = true;
        }
        return true;
    }
    const std::string stem = fileSafe(target->stem);
    const std::string dir = target->directory;
    auto candidate = [&](int n){
        return (dir.empty() ? std::string() : dir + "/") + stem + (n > 1 ? " " + std::to_string(n) : std::string()) + VAT_EXTENSION;
    };
    for (int n = 1;; n++){
        const std::string relative = candidate(n);
        const fs::path absolute = (project->getProjectPath() / FileUtils::pathFromUtf8(relative)).lexically_normal();
        const bool vatThere = fileExists(absolute);
        const bool meshThere = fileExists(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))));
        if (!vatThere && !meshThere){
            vatPath = relative;
            return true;
        }
        VatData old;
        if (vatThere && readVatFile(absolute, old, nullptr) && old.recipe.sources.size() == 1 && sameRecipeSource(old.recipe.sources[0], ours)){
            vatPath = relative;
            rewrite = true;
            return true;
        }
    }
}

bool VatBakeSession::write(VatWriteResult& out, std::string& error){
    out = VatWriteResult();
    if (bakeState != VatBakeState::Done){
        error = "nothing baked yet: bake first";
        return false;
    }
    std::string relative;
    bool rewrite = false;
    if (!plannedOutput(relative, rewrite, error)) return false;
    const fs::path absolute = (project->getProjectPath() / FileUtils::pathFromUtf8(relative)).lexically_normal();
    const fs::path dir = absolute.parent_path();
    const std::string stem = FileUtils::pathToUtf8(absolute.stem());

    VatData data = bakeResult.data;
    VatData old;
    const bool hasOld = rewrite && readVatFile(absolute, old, nullptr);
    data.guid = hasOld && !old.guid.empty() ? old.guid : AssetDatabase::generateGuid();
    const bool sameSource = hasOld && old.recipe.sources.size() == 1 && sameRecipeSource(old.recipe.sources[0], data.recipe.sources[0]);

    // the hand edits a rebake keeps (spec 4.5)
    if (hasOld && (rebakeLoaded || sameSource)){
        std::vector<int> match(data.clips.size(), -1);
        if (rebakeLoaded){
            for (size_t i = 0; i < data.clips.size() && i < bakeResult.sourceClips.size(); i++){
                for (const auto& entry : rebakeClipMap){
                    if (entry.first == bakeResult.sourceClips[i]) match[i] = entry.second;
                }
            }
        }else{
            std::vector<std::string> names;
            std::vector<float> lengths;
            for (const VatClip& clip : data.clips){
                names.push_back(clip.name);
                lengths.push_back(clip.length());
            }
            match = matchClips(old.clips, names, lengths);
        }
        std::set<std::string> used;
        for (size_t i = 0; i < data.clips.size(); i++){
            VatClip& clip = data.clips[i];
            if (match[i] < 0 || (size_t)match[i] >= old.clips.size()){
                used.insert(clip.name);
                continue;
            }
            const VatClip& previous = old.clips[(size_t)match[i]];
            if (!used.count(previous.name)) clip.name = previous.name;
            clip.events = previous.events;
            clip.speed = previous.speed;
            clip.interpolate = previous.interpolate;
            if ((previous.wrap == ClipWrapMode::Loop) == (clip.wrap == ClipWrapMode::Loop)) clip.wrap = previous.wrap;
            used.insert(clip.name);
        }
        for (VatSocket& socket : data.sockets){
            for (const VatSocket& previous : old.sockets){
                if (previous.path == socket.path) socket.name = previous.name;
            }
        }
        data.unknownKeys = old.unknownKeys;
    }

    // embedded textures next to the .vat
    std::vector<CreateVatCmd::File> files;
    std::vector<std::string> images;
    for (const VatEmbeddedImage& image : bakeResult.embeddedImages){
        fs::path file;
        for (int n = 1;; n++){
            file = dir / FileUtils::pathFromUtf8(stem + "_" + image.name + (n > 1 ? " " + std::to_string(n) : std::string()) + image.extension);
            std::string current;
            // a free name, the same bytes, or (rebaking) the file this bake wrote before
            if (!fileExists(file) || rewrite || (clipui::readWholeFile(file, current) && current == image.bytes)) break;
        }
        images.push_back(FileUtils::pathToUtf8(file));
        files.push_back({file, image.bytes});
    }
    VatMeshData mesh = bakeResult.mesh;
    resolveEmbedded(mesh, images);

    VatEncodedFiles encoded;
    if (!encodeVatFiles(FileUtils::pathToUtf8(absolute), data, mesh, bakeResult.payload(), bakeResult.sockets, encoded, &error)) return false;
    files.insert(files.begin(), {FileUtils::pathFromUtf8(encoded.meshPath), std::string(encoded.glbBytes.begin(), encoded.glbBytes.end())});
    files.insert(files.begin(), {absolute, encoded.vatText});

    out.vat = relative;
    out.mesh = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(FileUtils::pathFromUtf8(encoded.meshPath)));
    for (const CreateVatCmd::File& file : files) out.files.push_back(FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file.path)));
    out.rewritten = rewrite;
    out.guid = data.guid;
    out.undoName = "Bake Vertex Animation '" + relative + "'";

    if (bakeSpec.place == VatPlace::None){
        std::vector<WriteFilesCmd::Item> items;
        for (CreateVatCmd::File& file : files) items.push_back({file.path, std::move(file.content)});
        Project* owner = project;
        auto failureText = std::make_shared<std::string>();
        auto* command = new WriteFilesCmd(std::move(items), [owner](const std::vector<fs::path>& paths){
            CreateVatCmd::notifyFiles(owner, paths);
        });
        command->reportFailureTo(failureText);
        if (!project->getProjectCommandHistory()->addCommand(command)){
            error = failureText->empty() ? std::string("the files could not be written") : *failureText;
            return false;
        }
        return true;
    }

    // a scene step: the files and the placed GameObject
    VatPlacement placement;
    placement.mode = bakeSpec.place == VatPlace::Replace ? VatPlacement::Mode::Replace
                   : bakeSpec.place == VatPlace::Crowd ? VatPlacement::Mode::Crowd : VatPlacement::Mode::Beside;
    if (placement.mode == VatPlacement::Mode::Crowd){
        error = "crowd placement is not available yet: bake with place beside, then make it a crowd";
        return false;
    }
    uint32_t sceneId = 0;
    if (!target->scratch()){
        sceneId = target->sceneId;
        Scene* scene = target->scene;
        const Transform& transform = scene->getComponent<Transform>(target->root);
        placement.parent = transform.parent;
        placement.position = transform.position;
        placement.rotation = transform.rotation;
        placement.scale = transform.scale;
        placement.source = target->root;
        placement.name = scene->getEntityName(target->root) + " (VAT)";
    }else{
        if (placement.mode == VatPlacement::Mode::Replace){
            error = "place replace needs a GameObject source";
            return false;
        }
        sceneId = project->getSelectedSceneId();
        placement.name = target->stem + " (VAT)";
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        error = "no open scene to place the result in";
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        error = "the scene is playing: stop play mode to place the result";
        return false;
    }
    placement.model = ClipConverters::assetRelative(project, FileUtils::pathFromUtf8(encoded.meshPath));
    placement.animation = ClipConverters::assetRelative(project, absolute);
    placement.guid = data.guid;
    auto* command = new CreateVatCmd(project, sceneId, files, placement);
    auto failureText = std::make_shared<std::string>();
    command->reportFailureTo(failureText);
    CommandHistory* history = CommandHandle::get(sceneId);
    if (!history->addCommand(command)){
        error = failureText->empty() ? std::string("the bake could not be placed") : *failureText;
        return false;
    }
    out.sceneId = sceneId;
    out.entity = command->getEntity();
    return true;
}

// ---- staleness and rebake ----

std::vector<VatIssue> checkVatStaleness(Project* project, const std::string& vatPath){
    std::vector<VatIssue> out;
    if (!project) return out;
    fs::path given = FileUtils::pathFromUtf8(vatPath);
    const fs::path absolute = (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
    const std::string relative = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute));
    const std::string rebake = "Rebake (Inspector > Rebake, or bake_vertex_animation {rebake: \"" + relative + "\"})";
    VatData data;
    std::string error;
    if (!readVatFile(absolute, data, &error)){
        out.push_back(makeIssue("vat-layout-mismatch", true, "'" + relative + "' can't be read: " + error, rebake));
        return out;
    }
    for (const VatRecipeSource& source : data.recipe.sources){
        for (const auto& hash : source.hashes){
            const fs::path file = (project->getProjectPath() / FileUtils::pathFromUtf8(hash.first)).lexically_normal();
            if (!fileExists(file)){
                out.push_back(makeIssue("vat-source-missing", false, "'" + hash.first + "', a source of '" + relative + "', is missing",
                                        "restore it, or bake from another source"));
                continue;
            }
            if (fileSha1(FileUtils::pathToUtf8(file)) != hash.second){
                out.push_back(makeIssue("vat-source-changed", false, "'" + hash.first + "' changed since '" + relative + "' was baked", rebake));
            }
        }
    }
    const fs::path mesh = (absolute.parent_path() / FileUtils::pathFromUtf8(data.mesh)).lexically_normal();
    std::string bytes;
    if (data.mesh.empty() || !clipui::readWholeFile(mesh, bytes)){
        out.push_back(makeIssue("vat-mesh-mismatch", true, "the mesh '" + data.mesh + "' of '" + relative + "' is missing", rebake));
        return out;
    }
    VatGlbContents contents;
    const std::vector<uint8_t> glb(bytes.begin(), bytes.end());
    if (!readVatGlb(glb, contents, &error)){
        out.push_back(makeIssue("vat-mesh-mismatch", true, "the mesh '" + data.mesh + "' doesn't hold a payload: " + error, rebake));
        return out;
    }
    if (contents.layoutHash != data.layout.layoutHash || computeLayoutHash(data) != data.layout.layoutHash){
        out.push_back(makeIssue("vat-layout-mismatch", true, "'" + relative + "' and its mesh '" + data.mesh +
                                "' describe different layouts (a generated field changed, or the files come from two bakes)", rebake));
        return out;
    }
    if (!VertexAnimation::create(data, contents.payload, contents.sockets, &error)){
        out.push_back(makeIssue("vat-mesh-mismatch", true, "the payload of '" + data.mesh + "' doesn't match '" + relative + "': " + error, rebake));
    }
    return out;
}

bool rebakeVat(Project* project, const std::string& vatPath, VatWriteResult& out, std::string& error, std::vector<VatIssue>* warnings){
    VatBakeSession session(project);
    if (!session.loadRebake(vatPath, error)) return false;
    if (!session.run()){
        error = session.failureIssues().empty() ? session.error() : issuesText(session.failureIssues(), true);
        return false;
    }
    if (warnings) *warnings = session.result()->report.warnings;
    return session.write(out, error);
}

}
