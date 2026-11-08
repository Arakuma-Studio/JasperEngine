// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Procedural Materials for agents (docs/specs/S90-procedural-material.md R34-R38), the `materials` group:
// preview_procedural_material shows any material (a .procmat evaluated, not baked; a .material; an image;
// a submesh's) as a contact sheet with S35-shaped checks, changing nothing; bake_procedural_material writes
// the PNGs and the .material as one undo step. Both run as jobs on the editor's own device (ProceduralRun)
// and read the checks from ProceduralChecks, as the Procedural Material window does.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "DocumentFile.h"
#include "EditorHost.h"
#include "Project.h"
#include "Stream.h"
#include "command/CommandHistory.h"
#include "component/MeshComponent.h"
#include "graph/Graph.h"
#include "pose/PoseImage.h"
#include "procmat/NodeLibrary.h"
#include "procmat/ProceduralBake.h"
#include "procmat/ProceduralChecks.h"
#include "procmat/ProceduralMaterialKind.h"
#include "procmat/ProceduralDevice.h"
#include "procmat/ProceduralRun.h"
#include "texsynth/SynthesisSession.h"
#include "texsynth/SynthImage.h"
#include "texture/TextureImportSettings.h"
#include "util/AssetDatabase.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/QaChecks.h"
#include "util/SafeWrite.h"
#include "util/Util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace jasperengine::editor::mcp {

namespace procmattools {

namespace fs = std::filesystem;
using namespace jasperengine::editor::procmat;

constexpr int CELL = 192;
constexpr int LABEL = 14;
const std::vector<std::string> SHEET_PARTS = {"flat", "sphere", "tiled", "channels"};

// the runs in flight: kept alive until they end (a run is never destroyed inside its own callback)
std::vector<std::shared_ptr<ProceduralRun>>& liveRuns() {
    static std::vector<std::shared_ptr<ProceduralRun>> runs;
    return runs;
}

// R34: every result names the device, a refusal before anything ran too
Json deviceOfEditor() {
    const EvaluatorDevice device = procmat::editorDevice();
    return {{"tier", evaluatorTierName(device.tier)}, {"name", device.name}};
}

void refuse(const CallPtr& call, const std::string& message) {
    call->fail(message, Json{{"device", deviceOfEditor()}});
}

std::string relativeTo(Project* project, const fs::path& absolute) {
    return FileUtils::pathToGenericUtf8(absolute.lexically_relative(project->getProjectPath()));
}

std::string lowerExtension(const fs::path& path) {
    std::string extension = FileUtils::pathToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return extension;
}

bool loadRgba(const fs::path& file, const std::string& name, OutputImage& out, std::string& error) {
    texsynth::Image image;
    if (!texsynth::loadImage(file, image, error, nullptr, 8192)) return false;
    const texsynth::Image rgba = texsynth::toRgba8(image);
    out.name = name;
    out.width = rgba.width;
    out.height = rgba.height;
    out.rgba = rgba.data;
    return true;
}

// What a preview looks at
struct Source {
    std::string scope;          // the path, or the submesh: what fix rounds are counted by
    std::string kind;           // procmat, material, image, submesh
    graph::Graph graph;         // procmat
    std::string graphPath;      // procmat: project-relative
    Material material;          // material, submesh: the ball draws it
    MaterialImages images;      // material, image, submesh: what the checks read
};

// The image a texture slot of a material holds, if it can be read
bool slotImage(Project* project, const Texture& texture, const std::string& name, OutputImage& out) {
    const std::string path = texture.getPath();
    if (texture.empty() || path.empty()) return false;
    std::string ignored;
    return loadRgba(project->getAssetsPath() / FileUtils::pathFromUtf8(path), name, out, ignored);
}

// A material's images by the bake's names, and the shader texture that reads a baked height
void materialImages(Project* project, const Material& material, MaterialImages& images) {
    OutputImage image;
    if (slotImage(project, material.baseColorTexture, "base_color", image)) images.outputs["base_color"] = image;
    if (slotImage(project, material.normalTexture, "normal", image)) images.outputs["normal"] = image;
    if (slotImage(project, material.emissiveTexture, "emission", image)) images.outputs["emission"] = image;
    OutputImage orm;
    const bool mr = slotImage(project, material.metallicRoughnessTexture, "orm", orm);
    OutputImage occlusion;
    const bool ao = slotImage(project, material.occlusionTexture, "occlusion", occlusion);
    if (mr) {
        // one ORM in both slots, or a separate occlusion of the same size folded into red
        if (!(material.occlusionTexture.getPath() == material.metallicRoughnessTexture.getPath())) {
            const bool fits = ao && occlusion.width == orm.width && occlusion.height == orm.height;
            for (size_t i = 0; i < (size_t)orm.width * orm.height; i++) orm.rgba[i * 4] = fits ? occlusion.rgba[i * 4] : 255;
        }
        images.outputs["orm"] = orm;
    }
    for (const auto& [uniform, texture] : material.shaderTextures) {
        const fs::path file = project->getAssetsPath() / FileUtils::pathFromUtf8(texture.getPath());
        const std::optional<BakeRecipe> recipe = texture.getPath().empty() ? std::nullopt : readBakeRecipe(file);
        if (!recipe || recipe->output != "height") continue;
        std::string ignored;
        if (loadRgba(file, "height", image, ignored)) {
            images.outputs["height"] = image;
            images.heightReader = "the shader texture " + uniform;
        }
    }
}

bool readMaterialFile(const fs::path& file, Material& material, std::string& error) {
    try {
        const YAML::Node node = DocumentFiles::read(file, DocumentKind::Material).root;
        if (!node || !node.IsMap()) {
            error = "not a material file (expected a YAML map)";
            return false;
        }
        material = Stream::decodeMaterial(node);
        return true;
    } catch (const std::exception& e) {
        error = std::string("can't read the material: ") + e.what();
        return false;
    }
}

bool resolveSource(const CallPtr& call, Source& source) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    if (args.contains("path")) {
        fs::path file;
        const std::string rel = args.value("path", std::string());
        if (!McpRefs::resolveProjectPath(project, rel, file, error, true)) {
            refuse(call, error);
            return false;
        }
        source.scope = relativeTo(project, file);
        const std::string extension = lowerExtension(file);
        if (extension == PROCEDURAL_MATERIAL_EXTENSION) {
            source.kind = "procmat";
            source.graphPath = source.scope;
            if (!readGraphForBake(file, source.scope, source.graph, error)) {
                refuse(call, error);
                return false;
            }
            return true;
        }
        if (extension == ".material") {
            source.kind = "material";
            if (!readMaterialFile(file, source.material, error)) {
                refuse(call, source.scope + ": " + error);
                return false;
            }
            materialImages(project, source.material, source.images);
            return true;
        }
        if (Util::isImageFile(FileUtils::pathToUtf8(file))) {
            source.kind = "image";
            // what the image is for: its import type says
            TextureImportSettings settings;
            TextureImportSettings::load(FileUtils::pathToUtf8(file), settings);
            const TextureImportType type = settings.resolvedType();
            const std::string role = type == TextureImportType::Normal ? "normal" : (type == TextureImportType::SingleChannel ? "height" : "base_color");
            OutputImage image;
            if (!loadRgba(file, role, image, error)) {
                refuse(call, source.scope + ": " + error);
                return false;
            }
            source.images.outputs[role] = image;
            Texture texture;
            texture.setPath(FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(file)));
            if (role == "normal") source.material.normalTexture = texture;
            else source.material.baseColorTexture = texture;
            return true;
        }
        refuse(call, source.scope + " is not a .procmat, a .material or an image.");
        return false;
    }
    if (args.contains("entity")) {
        SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!sceneProject || !sceneProject->scene) {
            refuse(call, error.empty() ? "The scene is not open." : error);
            return false;
        }
        const Entity entity = McpRefs::resolveEntity(sceneProject, args["entity"], error);
        if (entity == NULL_ENTITY) {
            refuse(call, error);
            return false;
        }
        const MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
        if (!mesh || mesh->numSubmeshes == 0) {
            refuse(call, "The GameObject has no Mesh component with submeshes.");
            return false;
        }
        const int submesh = args.value("submesh", 0);
        if (submesh < 0 || (unsigned int)submesh >= mesh->numSubmeshes) {
            refuse(call, "submesh must be 0.." + std::to_string(mesh->numSubmeshes - 1) + ".");
            return false;
        }
        source.kind = "submesh";
        source.material = mesh->submeshes[(size_t)submesh].material;
        source.scope = sceneProject->name + "/" + std::to_string(entity) + "/" + std::to_string(submesh);
        materialImages(project, source.material, source.images);
        return true;
    }
    refuse(call, "Give path (a .procmat, a .material or an image) or entity (and submesh) of a mesh.");
    return false;
}

// ---- the sheet

// a square cell of the image: box-filtered when it shrinks, nearest texels when it grows
PoseImage cellOf(const OutputImage& image, int side, bool grey) {
    PoseImage out(side, side, PoseColor(0, 0, 0, 255));
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            const int x0 = x * image.width / side, x1 = std::max(x0 + 1, (x + 1) * image.width / side);
            const int y0 = y * image.height / side, y1 = std::max(y0 + 1, (y + 1) * image.height / side);
            int sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    const uint8_t* p = &image.rgba[((size_t)sy * image.width + sx) * 4];
                    for (int c = 0; c < 4; c++) sum[c] += p[c];
                }
            }
            const int n = (x1 - x0) * (y1 - y0);
            const uint8_t r = (uint8_t)(sum[0] / n);
            out.setPixel(x, y, grey ? PoseColor(r, r, r, 255) : PoseColor(r, (uint8_t)(sum[1] / n), (uint8_t)(sum[2] / n), 255));
        }
    }
    return out;
}

struct SheetCell {
    std::string label;
    PoseImage image;
};

// Top row: flat, the lit ball, a 2 x 2 tiling, the reference; bottom row: one cell per output
PoseImage composeSheet(const std::vector<std::string>& parts, const std::vector<OutputImage>& outputs, const OutputImage& sphere,
                       const OutputImage* reference, Json& cells) {
    const auto wants = [&](const char* part) { return std::find(parts.begin(), parts.end(), part) != parts.end(); };
    const OutputImage* face = nullptr;
    for (const OutputImage& image : outputs) {
        if (image.name == "base_color") face = &image;
    }
    if (!face && !outputs.empty()) face = &outputs.front();
    std::vector<SheetCell> top, bottom;
    if (face && wants("flat")) top.push_back({"flat " + face->name, cellOf(*face, CELL, face->name == "height")});
    if (wants("sphere") && sphere.width > 0) {
        PoseImage ball(CELL, CELL, PoseColor(24, 26, 30, 255));
        PoseImage drawn;
        std::vector<uint8_t> rgba = sphere.rgba;
        if (drawn.assign(sphere.width, sphere.height, std::move(rgba))) ball.blendImage(drawn, (CELL - sphere.width) / 2, (CELL - sphere.height) / 2);
        top.push_back({"sphere (lit)", ball});
    }
    if (face && wants("tiled")) {
        const PoseImage quarter = cellOf(*face, CELL / 2, face->name == "height");
        PoseImage tiled(CELL, CELL);
        for (int ty = 0; ty < 2; ty++) {
            for (int tx = 0; tx < 2; tx++) tiled.blit(quarter, tx * CELL / 2, ty * CELL / 2);
        }
        top.push_back({"tiled 2x2", tiled});
    }
    if (reference && reference->width > 0) top.push_back({"reference", cellOf(*reference, CELL, false)});
    if (wants("channels")) {
        for (const OutputImage& image : outputs) bottom.push_back({image.name, cellOf(image, CELL, image.name == "height")});
    }
    const int columns = (int)std::max(top.size(), bottom.size());
    const int rows = (top.empty() ? 0 : 1) + (bottom.empty() ? 0 : 1);
    if (columns == 0 || rows == 0) return PoseImage();
    PoseImage sheet(columns * CELL, rows * (CELL + LABEL), PoseColor(16, 17, 20, 255));
    cells = Json::array();
    int row = 0;
    for (const std::vector<SheetCell>* line : {&top, &bottom}) {
        if (line->empty()) continue;
        for (size_t i = 0; i < line->size(); i++) {
            const int x = (int)i * CELL, y = row * (CELL + LABEL);
            sheet.blit((*line)[i].image, x, y);
            sheet.drawText(x + 3, y + CELL + 2, (*line)[i].label, PoseColor(220, 220, 220, 255));
            cells.push_back({{"label", (*line)[i].label}, {"x", x}, {"y", y}, {"size", CELL}});
        }
        row++;
    }
    return sheet;
}

// ---- the job

// The run as a job: `done` builds the reply from the finished run on the main thread
void runJob(const CallPtr& call, ProceduralRunRequest request, const std::string& kind, const std::string& title,
            std::function<ToolResult(ProceduralRun& run)> done) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    const bool wait = args.value("wait", true);
    const double timeout = args.value("timeout_seconds", 120.0);
    auto run = std::make_shared<ProceduralRun>(std::move(request));
    liveRuns().push_back(run);
    JobPtr job = ctx.jobs().create(kind, title);
    auto reply = std::make_shared<ToolResult>();
    std::weak_ptr<ProceduralRun> weak = run;
    job->setCancelHandler([weak]() {
        if (auto r = weak.lock()) r->cancel();
    });
    job->setProgress(-1.0, run->getStage());
    run->setOnStage([job](const std::string& stage) { job->setProgress(-1.0, stage); });
    run->setOnFinished([weak, job, reply, done]() {
        auto r = weak.lock();
        if (!r) return;
        if (r->isFailed()) {
            if (job->cancelRequested()) job->markCancelled("Cancelled; nothing was written.");
            else job->fail(r->getError(), Json{{"device", {{"tier", evaluatorTierName(r->tier())}, {"name", r->deviceName()}}}});
            *reply = ToolResult::error(r->getError(), job->result());
        } else {
            // the run ends from the editor's frame, not inside a tool call: its writes stay beneath the
            // project as the call's own would, and whatever goes wrong ends the job
            McpWriteScope writeScope;
            try {
                *reply = done(*r);
            } catch (const std::exception& e) {
                *reply = ToolResult::error(std::string("The run failed, nothing was written: ") + e.what());
            }
            Json result = reply->structured.is_object() ? reply->structured : Json::object();
            if (reply->isError) job->fail(reply->text, result);
            else job->succeed(reply->text, result);
        }
        // released after this frame: never inside its own callback
        getEditorHost().enqueueMainThreadTask([weak]() {
            auto& runs = liveRuns();
            runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const std::shared_ptr<ProceduralRun>& p) { return p == weak.lock(); }), runs.end());
        });
    });
    run->start();
    call->detach();
    // a little past the run's own bound: the run fails at timeout_seconds with its reason
    replyWhenJobDone(call, job, wait, timeout + 5.0, [reply](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) {
            return ToolResult::error(reply->text.empty() ? finished.message() : reply->text, result);
        }
        ToolResult out = *reply;
        out.structured = result;
        return out;
    });
}

std::filesystem::path pendingMarker(Project* project) {
    const char* marker = std::getenv("JASPER_TEST_PROCMAT_SPHERE_PENDING");
    return marker && *marker ? project->getProjectPath() / marker : fs::path();
}

bool sizeArg(const CallPtr& call, int fallback, int low, int high, int& size) {
    size = call->args().value("size", fallback);
    if (size < low || size > high || (size & (size - 1)) != 0) {
        refuse(call, "size is a power of two from " + std::to_string(low) + " to " + std::to_string(high) + ".");
        return false;
    }
    return true;
}

bool failOnArg(const CallPtr& call, std::string& failOn) {
    failOn = call->args().value("fail_on", std::string());
    if (!failOn.empty() && failOn != "fail" && failOn != "warn") {
        refuse(call, "fail_on is fail or warn.");
        return false;
    }
    return true;
}

Json deviceJson(const ProceduralRun& run) {
    return {{"tier", evaluatorTierName(run.tier())}, {"name", run.deviceName()}};
}

// R9: a device failure the run continued past on the CPU never goes unsaid
void addWarnings(const ProceduralRun& run, Json& data, std::string& text) {
    if (run.getWarnings().empty()) return;
    data["warnings"] = run.getWarnings();
    for (const std::string& warning : run.getWarnings()) text += "\nWarning: " + warning;
}

std::string checksText(const Json& report) {
    std::string text;
    for (const Json& row : report["checks"]) text += "\n" + qa::rowLine(row);
    const Json& cost = report["metrics"]["cost"];
    text += "\nCost: " + cost["textures"].dump() + " texture(s), desktop " + cost["desktop_mb"].dump() + " MB, mobile " + cost["mobile_mb"].dump() +
            " MB, web " + cost["web_mb"].dump() + " MB (estimate).";
    if (!report["thresholds"]["changed"].empty()) text += "\nThresholds changed from their defaults: " + report["thresholds"]["changed"].dump() + ".";
    return text;
}

// ---- preview_procedural_material

void previewTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    auto source = std::make_shared<Source>();
    if (!resolveSource(call, *source)) return;
    std::vector<std::string> parts = SHEET_PARTS;
    if (args.contains("sheet")) {
        parts.clear();
        for (const Json& part : args["sheet"]) {
            const std::string name = part.is_string() ? part.get<std::string>() : std::string();
            if (std::find(SHEET_PARTS.begin(), SHEET_PARTS.end(), name) == SHEET_PARTS.end()) {
                refuse(call, "sheet holds flat, sphere, tiled and channels ([] for metrics only).");
                return;
            }
            parts.push_back(name);
        }
    }
    int size = 0;
    if (!sizeArg(call, 512, 64, 4096, size)) return;
    // the graph's own seed unless the call names one; never written back
    const uint32_t seed = args.contains("seed") ? args.value("seed", 0u) : (source->kind == "procmat" ? graphSeed(source->graph) : 0u);
    std::vector<std::string> only;
    if (args.contains("checks")) {
        for (const Json& id : args["checks"]) {
            const std::string name = id.is_string() ? id.get<std::string>() : std::string();
            const std::vector<std::string>& ids = ProceduralChecks::checkIds();
            if (std::find(ids.begin(), ids.end(), name) == ids.end()) {
                std::string list;
                for (const std::string& known : ids) list += (list.empty() ? "" : ", ") + known;
                refuse(call, "checks: '" + name + "' is not a check (" + list + ").");
                return;
            }
            only.push_back(name);
        }
    }
    std::string failOn;
    if (!failOnArg(call, failOn)) return;
    auto reference = std::make_shared<OutputImage>();
    if (args.contains("reference")) {
        fs::path file;
        std::string error;
        if (!McpRefs::resolveProjectPath(project, args.value("reference", std::string()), file, error, true) || !loadRgba(file, "reference", *reference, error)) {
            refuse(call, "reference: " + error);
            return;
        }
    }
    fs::path saveTarget;
    if (args.contains("save_path")) {
        std::string error;
        const std::string value = args.value("save_path", std::string());
        if (lowerExtension(FileUtils::pathFromUtf8(value)) != ".png" || !McpRefs::resolveProjectWritePath(project, value, saveTarget, error, false)) {
            refuse(call, "save_path must be a project-relative .png path" + (error.empty() ? std::string(".") : ": " + error));
            return;
        }
    }

    const uint64_t generation = texsynth::projectGeneration();
    const fs::path root = project->getProjectPath();
    ProceduralRunRequest request;
    if (source->kind == "procmat") {
        PlanOptions options;
        options.size = size;
        options.seed = (double)seed;
        options.maxBufferSize = size;
        PassPlan plan;
        std::string error;
        if (!planGraph(source->graph, NodeLibrary::shipped(), materialBakeRoots(source->graph), options, plan, error)) {
            refuse(call, source->scope + " does not evaluate: " + error);
            return;
        }
        request.plan = std::move(plan);
    } else {
        if (source->images.outputs.empty() && std::find(parts.begin(), parts.end(), "sphere") == parts.end()) {
            refuse(call, source->scope + " has no texture the editor can read: nothing to measure.");
            return;
        }
        for (const auto& [name, image] : source->images.outputs) request.images.push_back(image);
        request.material = source->material;
    }
    request.sphere = std::find(parts.begin(), parts.end(), "sphere") != parts.end();
    request.timeoutSeconds = args.value("timeout_seconds", 120.0);
    request.pendingMarker = pendingMarker(project);
    runJob(call, std::move(request), "preview_procedural_material", "Preview Procedural Material " + source->scope,
           [=](ProceduralRun& run) {
        if (generation != texsynth::projectGeneration() || project->getProjectPath() != root) {
            return ToolResult::error("Another project was opened during the preview of " + source->scope + ".", Json{{"device", deviceJson(run)}});
        }
        MaterialImages images;
        images.heightReader = source->images.heightReader;
        for (const OutputImage& image : run.getOutputs()) images.outputs[image.name] = image;
        Json data;
        if (source->kind == "procmat") {
            // the window's rows: the same function
            EvaluationResult evaluated;
            evaluated.ok = true;
            evaluated.outputs = run.getOutputs();
            data = graphCheckReport(project, source->scope, source->graph, evaluated, only);
        } else {
            CheckRequest checks;
            checks.qa = project->getProcMatQaSettings();
            checks.only = only;
            checks.rounds = checkRounds().previous(source->scope);
            data = ProceduralChecks::report(images, checks);
        }
        checkRounds().record(source->scope, data["checks"]);
        data["device"] = deviceJson(run);
        data["source"] = {{"kind", source->kind}, {"scope", source->scope}};
        Json outputs = Json::array();
        for (const OutputImage& image : run.getOutputs()) outputs.push_back(image.name);
        data["outputs"] = outputs;
        if (source->kind == "procmat") {
            data["size"] = size;
            data["seed"] = seed;
        }
        std::string text = source->scope + ": " + std::to_string(run.getOutputs().size()) + " output(s)" +
                           (source->kind == "procmat" ? " at " + std::to_string(size) + " (seed " + std::to_string(seed) + ")" : std::string()) + " on " +
                           evaluatorTierName(run.tier()) + " (" + run.deviceName() + "). Verdict " + data.value("verdict", "") + ".";
        text += checksText(data);
        addWarnings(run, data, text);
        if (reference->width > 0) {
            data["reference"] = ProceduralChecks::reference(images, *reference);
            text += "\nReference (advisory, no status): palette " + data["reference"]["palette_delta_e"].dump() + " dE, spectrum " +
                    data["reference"]["spectrum"].dump() + ", edges " + data["reference"]["edges"].dump() + ".";
        }
        text += "\n" + data.value("guidance", "");
        data["next"] = data.value("guidance", "");
        ToolResult reply;
        Json cells;
        const PoseImage sheet = composeSheet(parts, run.getOutputs(), run.getSphere(), reference->width > 0 ? reference.get() : nullptr, cells);
        std::vector<uint8_t> png;
        if (!sheet.isEmpty() && sheet.encodePng(png)) {
            data["image"] = {{"width", sheet.getWidth()}, {"height", sheet.getHeight()}, {"cells", cells}};
            if (!saveTarget.empty()) {
                std::string error;
                if (!safewrite::writeFileResolvedBeneath(McpRefs::rootFor(project, saveTarget), saveTarget, std::string(png.begin(), png.end()), error)) {
                    return ToolResult::error("Could not write the sheet to " + relativeTo(project, saveTarget) + ": " + error, data);
                }
                data["saved_to"] = relativeTo(project, saveTarget);
                text += "\nSaved to " + relativeTo(project, saveTarget) + ".";
            }
            reply.images.push_back({Base64::encode(png.data(), png.size()), "image/png"});
        }
        reply.text = text;
        reply.structured = data;
        reply.isError = qa::reaches(data.value("verdict", ""), failOn);
        return reply;
    });
}

// ---- bake_procedural_material

void bakeTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fs::path file;
    std::string error;
    const std::string given = args.value("path", std::string());
    if (!McpRefs::resolveProjectPath(project, given, file, error, true)) {
        refuse(call, error);
        return;
    }
    const std::string rel = relativeTo(project, file);
    if (lowerExtension(file) != PROCEDURAL_MATERIAL_EXTENSION) {
        refuse(call, rel + " is not a .procmat: bake_procedural_material bakes a Procedural Material graph.");
        return;
    }
    graph::Graph graphFile;
    if (!readGraphForBake(file, rel, graphFile, error)) {
        refuse(call, error);
        return;
    }
    const std::string folder = args.contains("output") ? args.value("output", std::string()) : defaultBakeFolder(rel);
    fs::path folderPath;
    if (!McpRefs::resolveProjectWritePath(project, folder + "/" + bakedMaterialName(bakeStem(rel)), folderPath, error, false)) {
        refuse(call, "output: " + error);
        return;
    }
    if (!project->isInsideAssetsPath(folderPath)) {
        refuse(call, "output must be a folder inside the assets directory (" + FileUtils::pathToGenericUtf8(project->getAssetsDir()) + ").");
        return;
    }
    BakeOptions options;
    if (!sizeArg(call, BAKE_SIZE_DEFAULT, BAKE_SIZE_MIN, BAKE_SIZE_MAX, options.size)) return;
    options.seed = args.contains("seed") ? args.value("seed", 0u) : graphSeed(graphFile);
    if (args.contains("outputs")) {
        for (const Json& name : args["outputs"]) options.outputs.push_back(name.is_string() ? name.get<std::string>() : name.dump());
    }
    std::string failOn;
    if (!failOnArg(call, failOn)) return;
    PassPlan plan;
    std::vector<std::string> outputs;
    if (!planBake(graphFile, NodeLibrary::shipped(), options, plan, outputs, error)) {
        refuse(call, rel + " does not bake: " + error + ".");
        return;
    }
    // the graph's identity: the database writes it into the file; one written a moment ago gets it now
    AssetDatabase::AssetInfo info;
    const std::string projectRel = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file));
    if (!project->getAssetDatabase().getInfo(projectRel, info)) {
        project->onAssetFilesChanged(project->getAssetDatabase().importAssets(file));
        project->getAssetDatabase().getInfo(projectRel, info);
    }
    BakeWrite bake;
    bake.graphPath = rel;
    bake.folder = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(folderPath.parent_path()));
    bake.recipe.graphGuid = info.guid.str();
    bake.recipe.graphPath = rel;
    bake.recipe.inputHash = bakeInputHash(plan);
    bake.recipe.seed = options.seed;
    bake.recipe.size = options.size;
    bake.overwrite = args.value("overwrite", false);
    bake.graphOutputs = graphBakeOutputs(graphFile);
    bake.recipe.graphOutputs = bake.graphOutputs;
    bake.recipe.seedFromGraph = !args.contains("seed");
    // the project it was asked in: a bake finishing after a switch writes nothing (synthesize_texture's rule)
    const uint64_t generation = texsynth::projectGeneration();
    const fs::path root = project->getProjectPath();
    const std::string stem = bakeStem(rel);
    ProceduralRunRequest request;
    request.plan = std::move(plan);
    request.timeoutSeconds = args.value("timeout_seconds", 120.0);
    runJob(call, std::move(request), "bake_procedural_material", "Bake Procedural Material " + rel, [=](ProceduralRun& run) mutable {
        Json data = {{"device", deviceJson(run)}, {"size", options.size}, {"seed", options.seed}, {"outputs", outputs}};
        if (!failOn.empty()) {
            MaterialImages images;
            for (const OutputImage& image : run.getOutputs()) images.outputs[image.name] = image;
            CheckRequest checks;
            checks.qa = project->getProcMatQaSettings();
            const Json report = ProceduralChecks::report(images, checks);
            data["verdict"] = report["verdict"];
            data["checks"] = report["checks"];
            if (qa::reaches(report.value("verdict", ""), failOn)) {
                return ToolResult::error(rel + ": verdict " + report.value("verdict", "") + " reaches fail_on " + failOn + "; nothing was written." +
                                         checksText(report), data);
            }
        }
        if (generation != texsynth::projectGeneration() || project->getProjectPath() != root) {
            return ToolResult::error("Another project was opened while " + rel + " baked; nothing was written.", data);
        }
        bake.recipe.tier = evaluatorTierName(run.tier());
        bake.recipe.device = run.deviceName();
        bake.images = run.getOutputs();
        BakeFiles files;
        std::string why;
        if (!encodeBake(project, bake, files, why) || !writeBakeUndoable(project, files, stem, why, project->getProjectPath())) {
            return ToolResult::error("Nothing was written: " + why, data);
        }
        Json undo = {{"scope", "project"}};
        if (const UndoEntry* top = project->getProjectCommandHistory()->top()) {
            undo["id"] = top->info.id;
            undo["name"] = top->name();
        }
        data["images"] = files.images;
        data["material"] = files.material;
        data["undo"] = undo;
        data["next"] = "link_material {entity, material: \"" + files.material + "\"} puts it on a mesh; preview_procedural_material {path: \"" +
                       files.material + "\"} checks the baked files.";
        std::string text = "Baked " + rel + " at " + std::to_string(options.size) + " (seed " + std::to_string(options.seed) + ") on " +
                           evaluatorTierName(run.tier()) + " (" + run.deviceName() + "): " + std::to_string(files.images.size()) + " PNG(s) and " +
                           files.material + ", one undo step \"" + undo.value("name", std::string("Bake Procedural Material ") + stem) + "\"" +
                           (undo.contains("id") ? " (id " + undo["id"].dump() + ")" : std::string()) + ".";
        addWarnings(run, data, text);
        return ToolResult::success(text, data);
    });
}

} // namespace procmattools

void registerProceduralMaterialTools(McpRegistry& registry) {
    using namespace schema;
    {
        ToolDef tool;
        tool.name = "preview_procedural_material";
        tool.title = "Preview Procedural Material";
        tool.description =
            "Show a material changing nothing: a .procmat evaluated (not baked), a .material, an image or entity's submesh. A contact "
            "sheet (flat, lit sphere, 2x2 tiling, each output), metrics and checks with fix hints; sheet [] for metrics only. "
            "bake_procedural_material writes the files.";
        tool.inputSchema = object({
            {"path", assetPath("A .procmat, .material or image.")},
            {"entity", entityRef("A GameObject with a mesh.")},
            {"scene", sceneRef()},
            {"submesh", withDefault(integer("", 0, 255), 0)},
            {"sheet", array(stringEnum("", {"flat", "sphere", "tiled", "channels"}), "Default: all.")},
            {"size", withDefault(integer("A .procmat's evaluation size, a power of two.", 64, 4096), 512)},
            {"seed", integer("Default: the graph's metadata.seed.", 0, 2147483647)},
            {"checks", array(string(""), "Default: all.")},
            {"fail_on", stringEnum("", {"fail", "warn"})},
            {"reference", string("An image to compare with (advisory).")},
            {"save_path", string("Also write the sheet PNG.")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 1, 3600), 120)}}, std::vector<std::string>{});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.writesFiles = [](const Json& args) { return args.contains("save_path"); };
        tool.handler = procmattools::previewTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "bake_procedural_material";
        tool.title = "Bake Procedural Material";
        tool.description =
            "Bake a .procmat to PNGs (base color, normal, ORM, emission, height) and a .material, one undo step; a re-bake keeps the "
            ".material's other fields. Default output: a folder named after the graph beside it. Refused while the graph has errors. "
            "Check it with preview_procedural_material first.";
        tool.inputSchema = object({
            {"path", assetPath("A .procmat.")},
            {"output", string("Project-relative folder.")},
            {"size", withDefault(integer("A power of two.", 64, 8192), 1024)},
            {"seed", integer("Default: the graph's metadata.seed.", 0, 2147483647)},
            {"outputs", array(stringEnum("", {"base_color", "normal", "orm", "emission", "height"}), "Default: every connected one.")},
            {"fail_on", stringEnum("Write nothing when the checks reach it.", {"fail", "warn"})},
            {"overwrite", withDefault(boolean("Replace PNGs that are not this graph's."), false)},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 1, 3600), 120)}}, {"path"});
        tool.writesFiles = [](const Json&) { return true; };
        tool.allowedWhilePlaying = false;
        tool.activity = ToolActivity::Edit;
        tool.attributesUndo = true;
        tool.handler = procmattools::bakeTool;
        registry.addTool(std::move(tool));
    }
}

}
