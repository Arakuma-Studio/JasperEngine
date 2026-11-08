// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// generate_terrain and preview_noise_graph (T2 R63, R64, N12). Everything real happens in
// noise/TerrainGeneration and the noise library; here are the arguments, the jobs and the results.

#include "McpTools.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "Project.h"
#include "component/TerrainComponent.h"
#include "graph/Graph.h"
#include "jobs/JobSystem.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/TerrainGeneration.h"
#include "noise/TerrainPresets.h"
#include "util/Base64.h"
#include "util/ProjectChecks.h"

#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

constexpr int kMaxPreviewImages = 12;

Json toMcp(const graph::Json& value) {
    return Json::parse(value.dump());
}

Json issueJson(const GenerationIssue& issue) {
    Json out = {{"code", issue.code}, {"message", issue.message}, {"severity", issue.warning ? "warning" : "error"}};
    if (!issue.nodeId.empty()) out["node"] = issue.nodeId;
    return out;
}

const char* mapKindName(terrain::MapKind kind) {
    switch (kind) {
        case terrain::MapKind::Height: return "height";
        case terrain::MapKind::Blend: return "blend";
        case terrain::MapKind::Hole: return "hole";
        default: return "density";
    }
}

Json changedJson(const std::vector<terrain::MapChangeReport>& changed) {
    Json list = Json::array();
    for (const terrain::MapChangeReport& report : changed) {
        std::string map = mapKindName(report.map.kind);
        if (report.map.kind == terrain::MapKind::Blend || report.map.kind == terrain::MapKind::Density) map += ":" + std::to_string(report.map.index);
        list.push_back({{"map", map},
                        {"rect", {report.rect.x, report.rect.y, report.rect.w, report.rect.h}},
                        {"local", {report.localMin.x, report.localMin.y, report.localMax.x, report.localMax.y}},
                        {"texels", report.texels}});
    }
    return list;
}

std::string hashText(uint64_t hash) {
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// A grey PNG of normalized values, at most maxWidth wide (nearest texel)
std::string imageOf(const std::vector<float>& values, int width, int depth, int maxWidth, float low, float high) {
    if (values.empty() || width <= 0 || depth <= 0) return {};
    const int outWidth = maxWidth > 0 ? std::min(width, maxWidth) : width;
    const int outDepth = std::max(1, depth * outWidth / width);
    std::vector<unsigned char> pixels(static_cast<size_t>(outWidth) * static_cast<size_t>(outDepth));
    const float span = high > low ? high - low : 1.0f;
    for (int y = 0; y < outDepth; y++) {
        const int sy = y * depth / outDepth;
        for (int x = 0; x < outWidth; x++) {
            const int sx = x * width / outWidth;
            const float t = std::clamp((values[static_cast<size_t>(sy) * width + sx] - low) / span, 0.0f, 1.0f);
            pixels[static_cast<size_t>(y) * outWidth + x] = static_cast<unsigned char>(t * 255.0f + 0.5f);
        }
    }
    std::vector<unsigned char> png;
    const auto append = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<unsigned char>*>(context);
        const auto* bytes = static_cast<const unsigned char*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    if (!stbi_write_png_to_func(append, &png, outWidth, outDepth, 1, pixels.data(), outWidth)) return {};
    return Base64::encode(png.data(), png.size());
}

// path or preset (R63, R64): the graph, its display name and what it recommends
bool loadGraphArg(const CallPtr& call, const char* pathKey, graph::Graph& out, std::string& displayName) {
    noise::registerNoiseKind();
    const Json& args = call->args();
    const bool hasPath = args.contains(pathKey) && args[pathKey].is_string();
    const bool hasPreset = args.contains("preset") && args["preset"].is_string();
    if (hasPath == hasPreset) {
        call->fail(std::string("Pass one of ") + pathKey + " or preset.");
        return false;
    }
    std::string text;
    std::string where;
    if (hasPreset) {
        where = args["preset"].get<std::string>();
        text = TerrainPresets::json(where);
        if (text.empty()) {
            std::string names;
            for (const TerrainPreset& preset : TerrainPresets::list(true)) names += (names.empty() ? "" : ", ") + preset.id;
            call->fail("No preset '" + where + "'. Presets: " + names + ".");
            return false;
        }
    } else {
        fs::path file;
        std::string error;
        where = args[pathKey].get<std::string>();
        if (!McpRefs::resolveProjectPath(call->ctx().project(), where, file, error, true)) {
            call->fail(error.empty() ? "path is required." : error);
            return false;
        }
        if (!ProjectChecks::readFile(file, text)) {
            call->fail("Could not read " + where + ".");
            return false;
        }
    }
    graph::LoadReport report;
    out = graph::load(text, report);
    if (!report.ok) {
        call->fail(where + " is not a graph file: " + report.error);
        return false;
    }
    if (out.kind != "noise") {
        call->fail(where + " is a '" + out.kind + "' graph, not a Noise Graph.");
        return false;
    }
    displayName = TerrainGeneration::displayNameOf(out, where);
    return true;
}

std::vector<std::pair<std::string, float>> variablesArg(const Json& args) {
    std::vector<std::pair<std::string, float>> out;
    if (args.contains("variables") && args["variables"].is_object()) {
        for (const auto& [name, value] : args["variables"].items()) {
            if (value.is_number()) out.emplace_back(name, value.get<float>());
        }
    }
    return out;
}

// A graph error names its fix where a client shows it (R65)
void addNext(Json& data, const std::vector<GenerationIssue>& issues, const std::string& path) {
    for (const GenerationIssue& issue : issues) {
        if (issue.nodeId.empty()) continue;
        std::string hint = "edit_graph on node " + issue.nodeId;
        if (!path.empty()) hint += " of " + path;
        data["next"] = hint + ", then preview_noise_graph.";
        return;
    }
}

Json generationJson(const GenerationResult& result, uint32_t seed, float maxHeight) {
    Json hashes = Json::object();
    Json layers = Json::object();
    Json densities = Json::object();
    for (const GenerationOutputStats& output : result.outputs) {
        hashes[output.name] = hashText(output.hash);
        if (output.name.rfind("layer:", 0) == 0) layers[output.name.substr(6)] = output.coverage;
        if (output.name.rfind("density:", 0) == 0) densities[output.name.substr(8)] = output.coverage;
    }
    const GenerationStats& s = result.stats;
    Json stats = {{"height", {{"min", s.heightMin}, {"max", s.heightMax}, {"mean", s.heightMean}, {"p5", s.p5}, {"p50", s.p50}, {"p95", s.p95}}},
                  {"height_metres", {{"min", s.heightMin * maxHeight}, {"max", s.heightMax * maxHeight}, {"mean", s.heightMean * maxHeight},
                                     {"p5", s.p5 * maxHeight}, {"p50", s.p50 * maxHeight}, {"p95", s.p95 * maxHeight}}},
                  {"slope_histogram", s.slopeHistogram},
                  {"clipped_low", s.clippedLow}, {"clipped_high", s.clippedHigh},
                  {"layer_coverage", layers}, {"density_coverage", densities}};
    Json warnings = Json::array();
    for (const GenerationIssue& issue : result.warnings) warnings.push_back(issueJson(issue));
    return {{"seed", seed}, {"hash", hashes}, {"stats", stats}, {"changed", changedJson(result.changed)}, {"undo_name", result.undoName},
            {"plan", {{"ops", result.ops}, {"dead", result.deadNodes}, {"folded", result.foldedNodes}, {"peak_bytes", result.peakBytes}}},
            {"ms", {{"evaluate", result.evaluateMs}, {"erosion", result.erosionMs}, {"apply", result.applyMs}}},
            {"droplet_steps", result.dropletSteps},
            {"rect", {result.rect.x, result.rect.y, result.rect.w, result.rect.h}},
            {"dry_run", result.dryRun}, {"warnings", warnings}};
}

struct GenerateState {
    JobPtr job;
    std::shared_ptr<TerrainGeneration::Run> run;
    std::string image;
};

void generateTerrainTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene || !scene->scene) {
        call->fail(error.empty() ? "The scene is not loaded." : error);
        return;
    }
    if (scene->playState != ScenePlayState::STOPPED) {
        call->fail("Scene " + std::to_string(scene->id) + " is playing; stop play mode before generating.");
        return;
    }
    Entity entity = NULL_ENTITY;
    if (args.contains("entity") && !args["entity"].is_null()) {
        entity = McpRefs::resolveEntity(scene, args["entity"], error);
        if (entity == NULL_ENTITY) {
            call->fail(error);
            return;
        }
    } else {
        std::vector<Entity> terrains;
        for (Entity candidate : scene->entities) {
            if (scene->scene->findComponent<TerrainComponent>(candidate)) terrains.push_back(candidate);
        }
        if (terrains.size() == 1) {
            entity = terrains[0];
        } else {
            const std::vector<Entity> selected = project->getSelectedEntities(scene->id);
            if (selected.size() == 1 && scene->scene->findComponent<TerrainComponent>(selected[0])) {
                entity = selected[0];
            } else {
                call->fail(terrains.empty() ? "The scene has no terrain. Create one with create_entity type terrain." : "The scene has several terrains: pass entity.",
                           {{"issues", Json::array({{{"code", "no-terrain"}, {"severity", "error"}, {"message", "no single terrain"}}})}});
                return;
            }
        }
    }
    if (!scene->scene->findComponent<TerrainComponent>(entity)) {
        call->fail("\"" + scene->scene->getEntityName(entity) + "\" has no Terrain component.",
                   {{"issues", Json::array({{{"code", "no-terrain"}, {"severity", "error"}, {"message", "no Terrain component"}}})}});
        return;
    }

    GenerationRequest request;
    if (!loadGraphArg(call, "graph", request.graph, request.displayName)) return;
    const std::string graphPath = args.contains("graph") && args["graph"].is_string() ? args["graph"].get<std::string>() : std::string();
    request.seed = static_cast<uint32_t>(args.value("seed", 1LL));
    request.variables = variablesArg(args);
    request.feather = args.value("feather", 0.0f);
    request.resolution = args.value("resolution", 0);
    request.dryRun = args.value("dry_run", false);
    if (args.contains("region") && !args["region"].is_null()) {
        const Json& region = args["region"];
        if (!region.is_object() || !region.contains("min") || !region.contains("max") || !region["min"].is_array() || !region["max"].is_array() ||
            region["min"].size() != 2 || region["max"].size() != 2) {
            call->fail("region must be {min: [x, z], max: [x, z]} in terrain-local metres.",
                       {{"issues", Json::array({{{"code", "bad-region"}, {"severity", "error"}, {"message", "region's shape"}}})}});
            return;
        }
        GenerationRegion parsed;
        parsed.minX = region["min"][0].get<float>();
        parsed.minZ = region["min"][1].get<float>();
        parsed.maxX = region["max"][0].get<float>();
        parsed.maxZ = region["max"][1].get<float>();
        request.region = parsed;
    }
    if (args.contains("apply_to") && args["apply_to"].is_array()) {
        request.applyTo.height = request.applyTo.layers = request.applyTo.density = false;
        for (const Json& item : args["apply_to"]) {
            const std::string name = item.is_string() ? item.get<std::string>() : std::string();
            if (name == "height") request.applyTo.height = true;
            else if (name == "layers") request.applyTo.layers = true;
            else if (name == "density") request.applyTo.density = true;
            else {
                call->fail("apply_to takes height, layers and density, not '" + name + "'.");
                return;
            }
        }
    }
    const int maxWidth = std::clamp(args.value("max_width", 512), 16, 4096);
    const uint32_t seed = request.seed;
    const bool dryRun = request.dryRun;
    const float maxHeight = scene->scene->getComponent<TerrainComponent>(entity).maxHeight;

    auto state = std::make_shared<GenerateState>();
    state->job = call->ctx().jobs().create("generate_terrain", "Generate Terrain (" + request.displayName + ", seed " + std::to_string(seed) + ")");
    bool refused = false;
    GenerationResult refusedResult;
    auto started = std::make_shared<bool>(false);
    state->run = TerrainGeneration::start(project, scene->id, entity, std::move(request),
        [state, seed, dryRun, maxWidth, maxHeight, graphPath, started, &refused, &refusedResult](const GenerationResult& result) {
            if (!*started) {
                // prepare refused it: answered at once, with no job time (R32)
                refused = true;
                refusedResult = result;
                return;
            }
            Json data = generationJson(result, seed, maxHeight);
            if (result.cancelled) {
                state->job->markCancelled("Generation cancelled; the terrain is untouched.");
                return;
            }
            if (!result.ok) {
                Json issues = Json::array();
                for (const GenerationIssue& issue : result.issues) issues.push_back(issueJson(issue));
                data["issues"] = issues;
                data["code"] = result.errorCode;
                addNext(data, result.issues, graphPath);
                state->job->fail(result.error.empty() ? "Generation failed." : result.error, data);
                return;
            }
            if (dryRun && !result.heights.empty()) {
                state->image = imageOf(result.heights, result.width, result.depth, maxWidth, 0.0f, 1.0f);
            }
            state->job->succeed(result.undoName + (dryRun ? " (dry run: nothing was written)" : "") + ": height " +
                                std::to_string(result.stats.heightMin) + " to " + std::to_string(result.stats.heightMax) + ", " +
                                std::to_string(result.changed.size()) + " map(s) changed.", data);
        });
    if (refused) {
        state->job->fail(refusedResult.error);
        Json data = generationJson(refusedResult, seed, maxHeight);
        Json issues = Json::array();
        for (const GenerationIssue& issue : refusedResult.issues) issues.push_back(issueJson(issue));
        data["issues"] = issues;
        data["code"] = refusedResult.errorCode;
        addNext(data, refusedResult.issues, graphPath);
        call->fail(refusedResult.error.empty() ? "The generation was refused." : refusedResult.error, data);
        return;
    }
    *started = true;
    std::weak_ptr<GenerateState> weak = state;
    state->job->setCancelHandler([weak]() {
        if (auto s = weak.lock()) {
            if (s->run) s->run->cancel();
        }
    });
    state->job->setProgress(0.0, "generating");
    call->detach();
    replyWhenJobDone(call, state->job, args.value("wait", true), args.value("timeout_seconds", 120.0), [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(finished.message(), result);
        if (!state->image.empty()) reply.images.push_back(ToolImage{state->image, "image/png"});
        return reply;
    });
}

struct PreviewState {
    JobPtr job;
    jobs::CancelToken cancel;
    std::vector<ToolImage> images;
};

void previewNoiseGraphTool(const CallPtr& call) {
    const Json& args = call->args();
    graph::Graph graphData;
    std::string displayName;
    if (!loadGraphArg(call, "path", graphData, displayName)) return;

    // ops, as edit_graph would make them, in memory (R64)
    if (args.contains("ops") && args["ops"].is_array()) {
        size_t index = 0;
        try {
            for (const Json& item : args["ops"]) {
                graph::apply(graphData, graph::opFromJson(graph::Json::parse(item.dump())));
                index++;
            }
        } catch (const std::exception& e) {
            call->fail("ops[" + std::to_string(index) + "]: " + e.what() + ".");
            return;
        }
    }
    const uint32_t seed = static_cast<uint32_t>(args.value("seed", 1LL));
    const int size = args.value("size", 128);
    if (size < 16 || size > 512) {
        call->fail("size must be 16..512, not " + std::to_string(size) + ".");
        return;
    }
    const double extent = args.value("extent", 1.0);
    if (!(extent > 0.0)) {
        call->fail("extent must be above 0.");
        return;
    }
    const std::string imagesMode = args.value("images", std::string("outputs"));
    if (imagesMode != "none" && imagesMode != "outputs" && imagesMode != "all") {
        call->fail("images must be none, outputs or all.");
        return;
    }
    const int maxWidth = std::clamp(args.value("max_width", 256), 16, 1024);

    float terrainSize = 1000.0f, maxHeight = 100.0f;
    std::string space = "terrain";
    auto preset = graphData.metadata.find("preset");
    if (preset != graphData.metadata.end() && preset->is_object()) {
        terrainSize = preset->value("terrain_size", terrainSize);
        maxHeight = preset->value("max_height", maxHeight);
    }
    auto meta = graphData.metadata.find("noise");
    if (meta != graphData.metadata.end() && meta->is_object()) space = meta->value("space", space);

    noise::CompileOptions options;
    options.previewNodes = true;
    options.resolution = size;
    options.maxHeight = maxHeight;
    auto variables = std::make_shared<std::vector<std::pair<std::string, float>>>(variablesArg(args));
    options.variables = *variables;
    noise::CompileResult compiled = noise::compile(graphData, options);
    Json issues = Json::array();
    bool anyError = false;
    for (const graph::Issue& issue : compiled.issues) {
        issues.push_back(toMcp(graph::toJson(issue)));
        anyError = anyError || issue.severity == graph::Severity::Error;
    }
    if (!compiled.graph || anyError) {
        std::string first = "The graph has errors.";
        std::string node;
        for (const graph::Issue& issue : compiled.issues) {
            if (issue.severity == graph::Severity::Error) {
                first = issue.message;
                node = issue.nodeId;
                break;
            }
        }
        Json data = {{"issues", issues}};
        if (!node.empty()) data["next"] = "edit_graph on node " + node + ", then preview_noise_graph.";
        call->fail(first, data);
        return;
    }

    auto state = std::make_shared<PreviewState>();
    state->job = call->ctx().jobs().create("preview_noise_graph", "Preview " + displayName);
    std::weak_ptr<PreviewState> weak = state;
    state->job->setCancelHandler([weak]() {
        if (auto s = weak.lock()) s->cancel.cancel();
    });
    std::shared_ptr<const noise::CompiledGraph> plan = compiled.graph;
    std::vector<graph::Issue> issueList = compiled.issues;

    // A newer preview of the same graph supersedes an older one still running (an agent tuning a slider)
    static std::map<std::string, std::weak_ptr<PreviewState>> running;
    const std::string key = args.contains("path") && args["path"].is_string() ? "path:" + args["path"].get<std::string>() : "preset:" + args.value("preset", std::string());
    if (auto older = running[key].lock()) {
        older->cancel.cancel();
        older->job->requestCancel();
    }
    running[key] = state;

    jobs::SubmitOptions submit;
    // Bounded (size <= 512) work an agent waits on: Lane::Normal, ahead of Background's Heavy compiles (Deviations)
    submit.lane = jobs::Lane::Normal;
    submit.name = "preview-noise-graph";
    // Finished from the worker (McpJob is thread-safe): a paused game's main-thread queue does not drain (N10)
    jobs::submit([state, plan, issueList, variables, seed, size, extent, space, terrainSize, maxHeight, imagesMode, maxWidth, displayName]() {
        const std::vector<noise::OutputInfo>& outputs = plan->outputs();
        noise::GridDomain domain;
        domain.width = domain.depth = size;
        domain.unitsPerTexel = (space == "world" ? static_cast<double>(terrainSize) : 1.0) * extent / static_cast<double>(size);
        noise::NodeProfile profile;
        noise::EvalReport report;
        noise::EvalContext context;
        context.seed = seed;
        context.maxHeight = maxHeight;
        context.metresPerTexel = static_cast<double>(terrainSize) * extent / static_cast<double>(size);
        context.cancel = &state->cancel;
        context.profile = &profile;
        context.report = &report;
        context.variables = *variables;
        context.uvPerUnit = 1.0 / (domain.unitsPerTexel * static_cast<double>(size));
        const size_t texels = static_cast<size_t>(size) * static_cast<size_t>(size);
        std::vector<std::vector<float>> values(outputs.size(), std::vector<float>(texels));
        std::vector<int> indices;
        std::vector<float*> targets;
        for (size_t i = 0; i < outputs.size(); i++) {
            indices.push_back(static_cast<int>(i));
            targets.push_back(values[i].data());
        }
        if (!plan->evaluateGridOutputs(domain, context, indices, targets)) {
            state->job->markCancelled("Preview cancelled.");
            return;
        }
        // Per node: its own first output, and the real outputs' names and hashes
        Json nodes = Json::array();
        Json outputHashes = Json::object();
        int imagesAdded = 0;
        for (const noise::CompiledNodeInfo& info : plan->nodes()) {
            Json node = {{"id", info.id}, {"type", info.type}, {"dead", info.dead}, {"folded", info.folded}};
            for (const noise::NodeProfileEntry& entry : profile.nodes) {
                if (entry.nodeId == info.id) {
                    node["ms"] = entry.ms;
                    node["ns_per_point"] = entry.nsPerPoint;
                    node["share"] = entry.share;
                }
            }
            for (size_t i = 0; i < outputs.size(); i++) {
                if (outputs[i].nodeId != info.id) continue;
                const noise::Interval analytic = plan->rangeOf(static_cast<int>(i));
                float low = values[i][0], high = values[i][0];
                double sum = 0.0;
                for (float value : values[i]) {
                    low = std::min(low, value);
                    high = std::max(high, value);
                    sum += value;
                }
                node["range_analytic"] = {analytic.min, analytic.max};
                node["range_observed"] = {low, high};
                node["mean"] = sum / static_cast<double>(texels);
                node["hash"] = hashText(noise::hashFloats(values[i].data(), texels));
                const bool isOutput = outputs[i].kind != noise::OutputKind::NodePreview;
                if (isOutput) outputHashes[outputs[i].name] = node["hash"];
                if (imagesMode != "none" && imagesAdded < kMaxPreviewImages && (imagesMode == "all" || isOutput)) {
                    const std::string image = imageOf(values[i], size, size, maxWidth, low, high);
                    if (!image.empty()) {
                        state->images.push_back(ToolImage{image, "image/png"});
                        node["image"] = imagesAdded;
                        imagesAdded++;
                    }
                }
                break;
            }
            Json own = Json::array();
            for (const graph::Issue& issue : issueList) {
                if (issue.nodeId == info.id) own.push_back(toMcp(graph::toJson(issue)));
            }
            node["issues"] = own;
            nodes.push_back(std::move(node));
        }
        Json data = {{"seed", seed}, {"size", size}, {"nodes", nodes}, {"outputs", outputHashes}, {"peak_bytes", report.peakBytes}};
        state->job->succeed(displayName + ": " + std::to_string(nodes.size()) + " node(s) at " + std::to_string(size) + ", seed " + std::to_string(seed) + ".", data);
    }, submit);

    state->job->setProgress(0.0, "previewing");
    call->detach();
    replyWhenJobDone(call, state->job, args.value("wait", true), args.value("timeout_seconds", 60.0), [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(finished.message(), result);
        reply.images = state->images;
        return reply;
    });
}

}

void registerTerrainGenerationTools(McpRegistry& registry) {
    using namespace schema;
    noise::registerNoiseKind();
    {
        ToolDef tool;
        tool.name = "generate_terrain";
        tool.title = "Generate Terrain";
        tool.description = "Generate a Terrain from a Noise Graph or preset as one undo step (a job). dry_run writes nothing and returns statistics and a "
                           "heightmap image. wait false returns job_id for get_job. Default seed 1, always echoed. Node graphs: jasper://docs/noise-graphs.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("The terrain; default the scene's only one, or the selected.")},
            {"graph", assetPath("A .noisegraph file.")},
            {"preset", string("")},
            {"seed", withDefault(integer(""), 1)},
            {"variables", anyObject("name -> number")},
            {"region", anyObject("{min: [x, z], max: [x, z]}, terrain-local metres")},
            {"feather", withDefault(number("Metres."), 0)},
            {"resolution", integer("", 2, 4096)},
            {"apply_to", array(string(""), "height, layers, density")},
            {"dry_run", withDefault(boolean(""), false)},
            {"max_width", withDefault(integer("", 16, 4096), 512)},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number(""), 120)}
        });
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.handler = generateTerrainTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "preview_noise_graph";
        tool.title = "Preview Noise Graph";
        tool.description = "Evaluate a Noise Graph without writing: per-node ranges, ms, dead and folded nodes, output hashes and images. ops previews edits "
                           "as edit_graph would make them. Needs no terrain and works in Play. To see a terrain's result use generate_terrain dry_run.";
        tool.inputSchema = object({
            {"path", assetPath("A .noisegraph file.")},
            {"preset", string("")},
            {"ops", array(anyObject("{op, ...args}"), "")},
            {"seed", withDefault(integer(""), 1)},
            {"size", withDefault(integer("", 16, 512), 128)},
            {"extent", withDefault(number("Terrain widths, or metres in world space."), 1)},
            {"images", withDefault(stringEnum("", {"none", "outputs", "all"}), "outputs")},
            {"max_width", withDefault(integer("", 16, 1024), 256)},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number(""), 60)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.handler = previewNoiseGraphTool;
        registry.addTool(std::move(tool));
    }
}

}
