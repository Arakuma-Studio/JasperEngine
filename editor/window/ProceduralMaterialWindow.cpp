// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralMaterialWindow.h"

#include "Backend.h"
#include "Engine.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "procmat/NodeLibrary.h"
#include "procmat/ProceduralBake.h"
#include "procmat/ProceduralDevice.h"
#include "procmat/ProceduralGpuEvaluator.h"
#include "procmat/ProceduralRun.h"
#include "procmat/ProceduralTieredEvaluator.h"
#include "texsynth/SynthesisSession.h"
#include "procmat/ProceduralMaterialKind.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "render/preview/MaterialRender.h"
#include "texture/Material.h"
#include "util/QaChecks.h"
#include "util/UIProbe.h"
#include "window/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using GraphJson = graph::Json;

namespace{

    // the window's own fields of a .procmat, in the graph's metadata (the setMetadata op)
    constexpr const char* META_SEED = "seed";
    constexpr const char* META_SEED_LOCKED = "seed_locked";
    constexpr const char* META_EXPOSED = "exposed";
    constexpr const char* META_PRESETS = "presets";
    constexpr const char* VARIATION_PREFIX = "variation:";
    constexpr int SEED_MAX = 2147483647;

    std::function<void()>& frameRequester(){
        static std::function<void()> requester;
        return requester;
    }

    double steadySeconds(){
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // FNV-1a: a node's image key, stable within a session
    uint64_t mixKey(uint64_t hash, const std::string& text){
        for (unsigned char c : text){
            hash ^= c;
            hash *= 1099511628211ull;
        }
        hash ^= 0xff;
        hash *= 1099511628211ull;
        return hash;
    }

    // SplitMix64, for the Variations' seeds: the same nine for one seed
    uint64_t splitMix(uint64_t x){
        x += 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }

    std::string typeLabelOf(const GraphDocument& document, const graph::Node& node){
        const graph::NodeTypeDesc* type = document.nodeType(node.type);
        return type && !type->displayName.empty() ? type->displayName : node.type;
    }

    const char* outputLabel(const std::string& output){
        if (output == "base_color") return "Base Color";
        if (output == "normal") return "Normal";
        if (output == "orm") return "ORM";
        if (output == "emission") return "Emission";
        if (output == "height") return "Height";
        return "Output";
    }

}

std::vector<ProceduralMaterialWindow*>& ProceduralMaterialWindow::windows(){
    static std::vector<ProceduralMaterialWindow*> all;
    return all;
}

ProceduralMaterialWindow::Services ProceduralMaterialWindow::defaultServices(){
    Services out;
    // R8's order: the editor's device as detected, the CPU tier after a pass fails there (R9)
    out.tierKnown = true;
    out.makeEvaluator = [](){ return procmat::makeEditorEvaluator(); };
    out.takeImage = [](procmat::ProceduralEvaluator& evaluator, const std::string& output) -> std::shared_ptr<TextureRender> {
        // the device's image while it draws; after a fallback the CPU tier's result holds the pixels
        auto* tiered = dynamic_cast<procmat::ProceduralTieredEvaluator*>(&evaluator);
        procmat::ProceduralEvaluator* device = tiered ? tiered->deviceEvaluator() : &evaluator;
        auto* gpu = dynamic_cast<procmat::ProceduralGpuEvaluator*>(device);
        std::unique_ptr<TextureRender> taken = gpu ? gpu->takeOutputTexture(output) : nullptr;
        if (!taken) return nullptr;
        return std::shared_ptr<TextureRender>(taken.release(), [](TextureRender* texture){
            texture->destroyTexture();
            delete texture;
        });
    };
    // preview_procedural_material's rows, fixes included
    out.checks = [](Project* project, const std::string& path, const graph::Graph& graph, const procmat::EvaluationResult& images){
        return procmat::graphCheckReport(project, path, graph, images);
    };
    // bake_procedural_material's bake: one run at a time per window, written as one undo step when it ends
    struct Bake{
        std::shared_ptr<procmat::ProceduralRun> run;
        BakeState state;
    };
    auto bake = std::make_shared<Bake>();
    out.startBake = [bake](Project* project, const BakeRequest& request, std::string& error){
        if (bake->run && !bake->run->isFinished()){
            error = "A bake is running.";
            return false;
        }
        const std::filesystem::path file = project->getProjectPath() / request.path;
        graph::Graph graph;
        if (!procmat::readGraphForBake(file, request.path, graph, error)) return false;
        procmat::BakeOptions options;
        options.size = request.size;
        options.seed = (uint32_t)request.seed;
        procmat::GraphBake prepared;
        if (!procmat::prepareGraphBake(project, file, request.path, graph, procmat::defaultBakeFolder(request.path), options, true, false, prepared, error)){
            return false;
        }
        // a file at an output's path that is not a bake of this graph (another graph's, a hand-made image) is
        // refused before the run, never replaced: the window has no overwrite
        for (const std::string& output : prepared.outputs){
            const std::string rel = prepared.write.folder + "/" + procmat::bakedImageName(prepared.stem, output);
            const std::filesystem::path png = project->getProjectPath() / rel;
            std::error_code ec;
            if (!std::filesystem::exists(png, ec)) continue;
            const std::optional<procmat::BakeRecipe> earlier = procmat::readBakeRecipe(png);
            if (!earlier || earlier->graphGuid != prepared.write.recipe.graphGuid){
                error = rel + " is not a bake of this graph: move or rename it, then Bake again.";
                return false;
            }
        }
        procmat::ProceduralRunRequest runRequest;
        runRequest.plan = prepared.plan;
        // a CPU tier's 2048² bake takes minutes (R25): the user cancels, no bound does
        runRequest.timeoutSeconds = 3600.0;
        auto run = std::make_shared<procmat::ProceduralRun>(std::move(runRequest));
        std::weak_ptr<procmat::ProceduralRun> weak = run;
        // the project it was asked in: a bake ending after a switch writes nothing (the tool's rule)
        const uint64_t generation = texsynth::projectGeneration();
        const std::filesystem::path root = project->getProjectPath();
        run->setOnStage([bake](const std::string& stage){ bake->state.message = stage; });
        run->setOnFinished([bake, weak, project, prepared, generation, root]() mutable {
            auto finished = weak.lock();
            if (!finished) return;
            bake->state.running = false;
            if (finished->isFailed()){
                bake->state.message = "Nothing was written: " + finished->getError();
                return;
            }
            if (generation != texsynth::projectGeneration() || project->getProjectPath() != root){
                bake->state.message = "Another project was opened while it baked; nothing was written.";
                return;
            }
            procmat::BakeFiles files;
            std::string why;
            if (!procmat::writeGraphBake(project, prepared, finished->getOutputs(), finished->tier(), finished->deviceName(), files, why)){
                bake->state.message = "Nothing was written: " + why;
                return;
            }
            bake->state.message = "Baked " + files.material + " and " + std::to_string(files.images.size()) + " PNG(s) on " +
                                  procmat::evaluatorTierName(finished->tier()) + " (" + finished->deviceName() + "), one undo step.";
            for (const std::string& warning : finished->getWarnings()) bake->state.message += " " + warning;
        });
        bake->run = run;
        bake->state = BakeState();
        bake->state.running = true;
        bake->state.progress = -1.0f;
        run->start();
        return true;
    };
    out.bakeState = [bake](){ return bake->state; };
    out.cancelBake = [bake](){
        if (bake->run && !bake->run->isFinished()) bake->run->cancel();
    };
    return out;
}

ProceduralMaterialWindow::ProceduralMaterialWindow(Project* project, Services given)
    : GraphEditorWindow(project, WINDOW_NAME, procmat::PROCEDURAL_MATERIAL_KIND), services(std::move(given)){
    library = procmat::NodeLibrary::shipped();
    thumbs = std::make_unique<procmat::ProcMatThumbnails>(library, services.makeEvaluator, services.takeImage);
    windows().push_back(this);
}

ProceduralMaterialWindow::~ProceduralMaterialWindow(){
    dropSessions();
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
    auto& all = windows();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

void ProceduralMaterialWindow::setFrameRequester(std::function<void()> requester){
    frameRequester() = std::move(requester);
}

bool ProceduralMaterialWindow::processFrame(){
    const double t = steadySeconds();
    bool busy = false;
    const std::vector<ProceduralMaterialWindow*> all = windows();
    for (ProceduralMaterialWindow* window : all){
        if (window->isOpen() && window->getDocument()){
            busy = window->update(t) || busy;
            continue;
        }
        // closed: it keeps no evaluator, image or texture (a disabled feature costs nothing)
        if (window->material) window->dropSessions();
        for (Texture& texture : window->retired) texture.destroy();
        window->retired.clear();
    }
    // the passes drawn here are committed as a view's are: a read back waits for the commit of the
    // frame that drew, and an editor whose scenes are idle commits nothing else
    if (busy && Engine::isViewLoaded()) SystemRender::commit();
    return busy;
}

void ProceduralMaterialWindow::releaseGpu(){
    for (ProceduralMaterialWindow* window : windows()){
        window->dropSessions();
        window->ball.reset();
        for (Texture& texture : window->retired) texture.destroy();
        window->retired.clear();
    }
}

// ---------------------------------------------------------------- sessions

void ProceduralMaterialWindow::ensureSessions(){
    if (material || !library || !services.makeEvaluator) return;
    procmat::PlanOptions options;
    options.size = PREVIEW_SIZE;
    options.readBack = true;
    options.maxBufferSize = PREVIEW_SIZE;
    options.seed = graphSeed();
    sessionSeed = options.seed;
    materialDevice = services.makeEvaluator();
    focusDevice = services.makeEvaluator();
    if (!materialDevice || !focusDevice) return;
    material = std::make_unique<procmat::ProceduralSession>(*materialDevice, library, options);
    focus = std::make_unique<procmat::ProceduralSession>(*focusDevice, library, options);
}

void ProceduralMaterialWindow::dropSessions(){
    // the sessions first: they hold their evaluators
    material.reset();
    focus.reset();
    materialDevice.reset();
    focusDevice.reset();
    if (thumbs) thumbs->clear();
    for (auto& [name, upload] : outputUploads) retire(upload);
    outputUploads.clear();
    retire(focusUpload);
    for (auto& [id, upload] : thumbUploads) retire(upload);
    thumbUploads.clear();
    materialSerial = focusSerial = 0;
    evaluationStarted = lastEvaluationSeconds = -1.0;
    requests.clear();
    workLeft = false;
    nodeKeys.clear();
    visible.clear();
    nextVisible.clear();
    report = nlohmann::json();
    reportProblems.clear();
}

bool ProceduralMaterialWindow::update(double time){
    now = time;
    // what the last frame drew is drawn: what it replaced can go
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
    if (!material) return false;
    bool busy = false;

    const bool stale = material->isStale();
    const bool pendingBefore = material->isRebuildPending();
    material->update(now, FRAME_BUDGET_MS * 0.5);
    // a rebuild started (its debounce ran out): the estimate times those, never a value edit, which
    // redraws only from the pass it changed
    if (pendingBefore && !material->isRebuildPending()) evaluationStarted = now;
    if (stale && !material->isStale() && material->hasImage()){
        materialSerial++;
        if (evaluationStarted >= 0.0) lastEvaluationSeconds = now - evaluationStarted;
        // the rows the agent's preview would give this image
        if (services.checks && getDocument()){
            report = services.checks(project, getDocument()->projectPath(), getDocument()->graph(), material->latest());
            reportProblems = problemsFromReport(report);
        }
    }
    if (!material->isStale()) evaluationStarted = -1.0;
    busy = busy || material->isRebuildPending() || (material->isStale() && material->error().empty());

    if (!shownTarget().node.empty() && focus){
        const bool focusStale = focus->isStale();
        focus->update(now, FRAME_BUDGET_MS * 0.25);
        if (focusStale && !focus->isStale() && focus->hasImage()) focusSerial++;
        busy = busy || focus->isRebuildPending() || (focus->isStale() && focus->error().empty());
    }
    busy = thumbs->update(FRAME_BUDGET_MS * 0.25) || busy;
    workLeft = busy;
    return busy;
}

double ProceduralMaterialWindow::graphSeed() const{
    return (double)seed();
}

const GraphJson& ProceduralMaterialWindow::metadata(const std::string& key) const{
    static const GraphJson none;
    if (!getDocument()) return none;
    const GraphJson& meta = getDocument()->graph().metadata;
    if (!meta.is_object()) return none;
    auto it = meta.find(key);
    return it == meta.end() ? none : *it;
}

bool ProceduralMaterialWindow::setMetadata(const std::string& key, const GraphJson& value, const std::string& undoName){
    if (!getDocument()) return false;
    if (metadata(key) == value) return true;
    return apply({{"setMetadata", {{"key", key}, {"value", value}}}}, undoName);
}

void ProceduralMaterialWindow::documentChanged(){
    const graph::Graph& current = getDocument()->graph();
    // a pin of a node that is gone
    for (int slot = 0; slot < PIN_SLOTS; slot++){
        if (!pins[(size_t)slot].node.empty() && !current.findNode(pins[(size_t)slot].node)) clearPin(slot);
    }
    // the rows (and their fixes) were of the image before this revision
    report = nlohmann::json();
    reportProblems.clear();
    // a closed window replans nothing: its frameStarted does when it shows again
    if (!isOpen()){
        dropSessions();
        return;
    }
    ensureSessions();
    if (!material) return;
    const graph::Graph& g = getDocument()->graph();
    const double s = graphSeed();
    if (s != sessionSeed){
        sessionSeed = s;
        procmat::PlanOptions options;
        options.size = PREVIEW_SIZE;
        options.readBack = true;
        options.maxBufferSize = PREVIEW_SIZE;
        options.seed = s;
        // a seed is a value edit: the passes take it as a uniform (R7)
        material->setOptions(options, now);
        focus->setOptions(options, now);
    }
    material->setGraph(g, now);
    focus->setGraph(g, now);
    thumbs->setGraph(g);
    computeKeys();
    syncFocusRoots();
}

void ProceduralMaterialWindow::documentClosed(){
    dropSessions();
    // the next graph's ball starts from no texture, not the last graph's
    if (ballMaterial) *ballMaterial = Material();
    pins = {};
    activePin = -1;
    variationsOpen = false;
    bakeError.clear();
}

void ProceduralMaterialWindow::selectionChanged(){
    syncFocusRoots();
}

void ProceduralMaterialWindow::syncFocusRoots(){
    if (!focus) return;
    const Target target = shownTarget();
    // the material's own image is the material session's
    if (!target.node.empty()) focus->setRoots(rootsOf(target), now);
}

// ---------------------------------------------------------------- what is shown

std::optional<procmat::PlanRoot> ProceduralMaterialWindow::thumbnailRoot(const graph::Node& node) const{
    if (!getDocument()) return std::nullopt;
    const graph::Graph& g = getDocument()->graph();
    if (node.type == procmat::MATERIAL_OUTPUT_NODE){
        // the material's: its Base Color
        for (const graph::Edge& edge : g.edges){
            if (edge.to.node == node.id && edge.to.port == "base_color") return procmat::PlanRoot{"base_color", node.id, "base_color"};
        }
        return std::nullopt;
    }
    if (!getDocument()->nodeType(node.type)) return std::nullopt;
    for (const graph::Port& port : graph::portsOf(g, node)){
        if (port.direction == graph::PortDirection::Out) return procmat::PlanRoot{"image", node.id, port.id};
    }
    return std::nullopt;
}

ProceduralMaterialWindow::Target ProceduralMaterialWindow::shownTarget() const{
    if (activePin >= 0 && !pins[(size_t)activePin].node.empty()) return pins[(size_t)activePin];
    if (!getDocument() || selectedNodes().empty()) return Target();
    const graph::Node* node = getDocument()->graph().findNode(selectedNodes().front());
    if (!node || node->type == procmat::MATERIAL_OUTPUT_NODE) return Target();
    const std::optional<procmat::PlanRoot> root = thumbnailRoot(*node);
    return root ? Target{root->node, root->port} : Target();
}

std::vector<procmat::PlanRoot> ProceduralMaterialWindow::rootsOf(const Target& target) const{
    if (target.node.empty()) return {};
    return {{"image", target.node, target.port}};
}

void ProceduralMaterialWindow::clickPin(int slot){
    if (slot < 0 || slot >= PIN_SLOTS) return;
    if (activePin == slot){
        // the slot shown: back to following the selection
        activePin = -1;
    }else if (pins[(size_t)slot].node.empty()){
        const Target target = shownTarget();
        if (target.node.empty()) return;
        pins[(size_t)slot] = target;
        activePin = slot;
    }else{
        activePin = slot;
    }
    syncFocusRoots();
}

void ProceduralMaterialWindow::clearPin(int slot){
    if (slot < 0 || slot >= PIN_SLOTS) return;
    pins[(size_t)slot] = Target();
    if (activePin == slot) activePin = -1;
    syncFocusRoots();
}

// ---------------------------------------------------------------- seed

int ProceduralMaterialWindow::seed() const{
    const GraphJson& value = metadata(META_SEED);
    if (!value.is_number()) return 0;
    const double v = value.get<double>();
    return std::isfinite(v) ? (int)std::clamp(v, 0.0, (double)SEED_MAX) : 0;
}

bool ProceduralMaterialWindow::seedLocked() const{
    const GraphJson& value = metadata(META_SEED_LOCKED);
    return value.is_boolean() && value.get<bool>();
}

bool ProceduralMaterialWindow::setSeed(int value){
    if (seedLocked()) return false;
    return setMetadata(META_SEED, std::clamp(value, 0, SEED_MAX), "Set Seed");
}

bool ProceduralMaterialWindow::setSeedLocked(bool locked){
    // unlocked is no key at all: a file nobody locked stays as it was
    return setMetadata(META_SEED_LOCKED, locked ? GraphJson(true) : GraphJson(), locked ? "Lock Seed" : "Unlock Seed");
}

bool ProceduralMaterialWindow::rerollSeed(){
    if (seedLocked()) return false;
    static std::mt19937 random{std::random_device{}()};
    std::uniform_int_distribution<int> pick(1, SEED_MAX);
    int next = pick(random);
    while (next == seed()) next = pick(random);
    return setMetadata(META_SEED, next, "Reroll Seed");
}

std::vector<int> ProceduralMaterialWindow::variationSeeds() const{
    std::vector<int> out;
    uint64_t state = (uint64_t)seed();
    for (int i = 0; i < VARIATIONS; i++){
        state = splitMix(state);
        int value = (int)(state % (uint64_t)SEED_MAX) + 1;
        if (value == seed()) value = value % SEED_MAX + 1;
        out.push_back(value);
    }
    return out;
}

bool ProceduralMaterialWindow::isVariationCurrent(size_t index) const{
    // asked for at its seed's key and drawn at it: between an edit and the frame that asks again, no cell is
    return thumbs->isCurrent(VARIATION_PREFIX + std::to_string(index));
}

bool ProceduralMaterialWindow::adoptVariation(size_t index){
    const std::vector<int> seeds = variationSeeds();
    // a click adopts the seed it pictures: not while the cell still shows other seeds' image
    if (index >= seeds.size() || seedLocked() || !isVariationCurrent(index)) return false;
    return setMetadata(META_SEED, seeds[index], "Use Variation Seed");
}

// ---------------------------------------------------------------- Exposed Properties and Presets

std::vector<ProceduralMaterialWindow::Exposed> ProceduralMaterialWindow::exposedProperties() const{
    std::vector<Exposed> out;
    const GraphJson& list = metadata(META_EXPOSED);
    if (!list.is_array() || !getDocument()) return out;
    for (const GraphJson& item : list){
        if (!item.is_object() || !item.contains("node") || !item.contains("param") || !item["node"].is_string() || !item["param"].is_string()) continue;
        const std::string node = item["node"].get<std::string>();
        const std::string param = item["param"].get<std::string>();
        // a node deleted since, or a parameter its type no longer has, is not shown
        const graph::Node* found = getDocument()->graph().findNode(node);
        const graph::NodeTypeDesc* type = found ? getDocument()->nodeType(found->type) : nullptr;
        if (!type || std::none_of(type->params.begin(), type->params.end(), [&](const graph::ParamDesc& p){ return p.name == param; })) continue;
        out.push_back({node, param});
    }
    return out;
}

bool ProceduralMaterialWindow::isExposed(const std::string& node, const std::string& param) const{
    for (const Exposed& e : exposedProperties()){
        if (e.node == node && e.param == param) return true;
    }
    return false;
}

bool ProceduralMaterialWindow::setExposed(const std::string& node, const std::string& param, bool exposed){
    if (!getDocument()) return false;
    const graph::Node* target = getDocument()->graph().findNode(node);
    const graph::NodeTypeDesc* type = target ? getDocument()->nodeType(target->type) : nullptr;
    if (exposed && (!type || std::none_of(type->params.begin(), type->params.end(), [&](const graph::ParamDesc& p){ return p.name == param; }))) return false;
    GraphJson list = GraphJson::array();
    bool found = false;
    const GraphJson& current = metadata(META_EXPOSED);
    if (current.is_array()){
        for (const GraphJson& item : current){
            const bool same = item.is_object() && item.value("node", std::string()) == node && item.value("param", std::string()) == param;
            found = found || same;
            if (!same || exposed) list.push_back(item);
        }
    }
    if (exposed && !found) list.push_back({{"node", node}, {"param", param}});
    if (exposed == found) return true;
    return setMetadata(META_EXPOSED, list.empty() ? GraphJson() : list, (exposed ? "Expose " : "Unexpose ") + paramLabel(param));
}

std::vector<std::string> ProceduralMaterialWindow::presetNames() const{
    std::vector<std::string> out;
    const GraphJson& list = metadata(META_PRESETS);
    if (!list.is_array()) return out;
    for (const GraphJson& item : list){
        if (item.is_object() && item.contains("name") && item["name"].is_string()) out.push_back(item["name"].get<std::string>());
    }
    return out;
}

bool ProceduralMaterialWindow::savePreset(const std::string& name){
    if (name.empty() || !getDocument()) return false;
    GraphJson values = GraphJson::object();
    for (const Exposed& e : exposedProperties()){
        const graph::Node* node = getDocument()->graph().findNode(e.node);
        const graph::NodeTypeDesc* type = getDocument()->nodeType(node->type);
        GraphJson value;
        for (const graph::ParamDesc& p : type->params){
            if (p.name == e.param) value = p.defaultValue;
        }
        if (auto it = node->params.find(e.param); it != node->params.end()) value = *it;
        values[e.node + "/" + e.param] = value;
    }
    GraphJson list = GraphJson::array();
    bool replaced = false;
    const GraphJson& current = metadata(META_PRESETS);
    if (current.is_array()){
        for (const GraphJson& item : current){
            if (item.is_object() && item.value("name", std::string()) == name){
                list.push_back({{"name", name}, {"values", values}});
                replaced = true;
            }else{
                list.push_back(item);
            }
        }
    }
    if (!replaced) list.push_back({{"name", name}, {"values", values}});
    return setMetadata(META_PRESETS, list, "Save Preset " + name);
}

bool ProceduralMaterialWindow::applyPreset(const std::string& name){
    const GraphJson& list = metadata(META_PRESETS);
    if (!list.is_array() || !getDocument()) return false;
    for (const GraphJson& item : list){
        if (!item.is_object() || item.value("name", std::string()) != name) continue;
        const auto values = item.find("values");
        if (values == item.end() || !values->is_object()) return false;
        std::vector<graph::GraphOp> ops;
        for (auto it = values->begin(); it != values->end(); ++it){
            const std::string& key = it.key();
            const size_t slash = key.rfind('/');
            if (slash == std::string::npos) continue;
            const std::string node = key.substr(0, slash);
            // a node deleted since keeps nothing of the preset from applying the rest
            if (!getDocument()->graph().findNode(node)) continue;
            ops.push_back({"setParam", {{"node", node}, {"name", key.substr(slash + 1)}, {"value", it.value()}}});
        }
        if (ops.empty()) return true;
        return apply(ops, "Apply Preset " + name);
    }
    return false;
}

bool ProceduralMaterialWindow::deletePreset(const std::string& name){
    GraphJson list = GraphJson::array();
    const GraphJson& current = metadata(META_PRESETS);
    if (!current.is_array()) return false;
    bool found = false;
    for (const GraphJson& item : current){
        if (item.is_object() && item.value("name", std::string()) == name) found = true;
        else list.push_back(item);
    }
    if (!found) return false;
    return setMetadata(META_PRESETS, list.empty() ? GraphJson() : list, "Delete Preset " + name);
}

// ---------------------------------------------------------------- device and bake

procmat::EvaluatorTier ProceduralMaterialWindow::tier() const{
    return materialDevice ? materialDevice->tier() : procmat::EvaluatorTier::Gpu;
}

bool ProceduralMaterialWindow::tierKnown() const{
    return services.tierKnown;
}

std::string ProceduralMaterialWindow::tierLabel() const{
    return tierKnown() ? procmat::evaluatorTierName(tier()) : "unknown";
}

procmat::EvaluatorTier ProceduralMaterialWindow::budgetTier() const{
    // a device nobody named may be a software renderer: R24's tier 2 budget, never every node per frame
    return tierKnown() ? tier() : procmat::EvaluatorTier::Software;
}

std::vector<std::string> ProceduralMaterialWindow::ballOutputs() const{
    std::vector<std::string> out;
    for (const auto& [name, upload] : outputUploads){
        if (upload.serial != 0) out.push_back(name);
    }
    return out;
}

std::string ProceduralMaterialWindow::deviceName() const{
    return materialDevice ? materialDevice->deviceName() : std::string();
}

std::string ProceduralMaterialWindow::tierReason() const{
    if (!tierKnown()) return std::string();
    switch (tier()){
        case procmat::EvaluatorTier::Cpu: return "No GPU available, baking on the CPU.";
        case procmat::EvaluatorTier::Software: return "No GPU available, baking on the software renderer.";
        case procmat::EvaluatorTier::Gpu: break;
    }
    return std::string();
}

double ProceduralMaterialWindow::bakeEstimateSeconds(int size) const{
    if (lastEvaluationSeconds < 0.0 || size <= 0) return -1.0;
    // the preview's time per pixel: tiers 2 and 3 are as fast per pixel at any size (R25's measures)
    const double scale = (double)size / (double)PREVIEW_SIZE;
    return lastEvaluationSeconds * scale * scale;
}

bool ProceduralMaterialWindow::canBake(std::string* why) const{
    const auto refuse = [&](const std::string& reason){
        if (why) *why = reason;
        return false;
    };
    if (!getDocument()) return refuse("No graph is open.");
    if (!services.startBake) return refuse("This editor has no bake.");
    if (services.bakeState && services.bakeState().running) return refuse("A bake is running.");
    // the graph's own errors refuse it; a quality check's FAIL is the user's (or the agent's) to weigh
    if (!getDocument()->loadError().empty()) return refuse("The graph does not load.");
    for (const graph::Issue& issue : getDocument()->issues()){
        if (issue.severity == graph::Severity::Error) return refuse("The graph has errors: fix them first.");
    }
    return true;
}

bool ProceduralMaterialWindow::bake(){
    std::string why;
    if (!canBake(&why)){
        bakeError = why;
        return false;
    }
    BakeRequest request;
    request.path = getDocument()->projectPath();
    request.size = bakeSizeValue;
    request.seed = seed();
    bakeError.clear();
    if (!services.startBake(project, request, bakeError)) return false;
    return true;
}

// ---------------------------------------------------------------- Problems

std::vector<GraphEditorWindow::Problem> ProceduralMaterialWindow::problemsFromReport(const nlohmann::json& report){
    std::vector<Problem> out;
    if (!report.is_object() || !report.contains("checks") || !report["checks"].is_array()) return out;
    const nlohmann::json fixes = report.contains("fixes") && report["fixes"].is_array() ? report["fixes"] : nlohmann::json::array();
    for (const nlohmann::json& row : report["checks"]){
        if (!row.is_object()) continue;
        const std::string status = row.value("status", std::string());
        if (status != "FAIL" && status != "WARN") continue;
        Problem problem;
        problem.severity = status == "FAIL" ? graph::Severity::Error : graph::Severity::Warning;
        problem.code = row.value("id", std::string());
        problem.message = qa::rowLine(row);
        if (row.contains("node") && row["node"].is_string()) problem.nodeId = row["node"].get<std::string>();
        // a check with more than one fix (two nodes named) is not a single edit: the agent's, through its fix_hint
        const auto same = [&](const nlohmann::json& fix){ return fix.is_object() && fix.value("check", std::string()) == problem.code; };
        const bool single = std::count_if(fixes.begin(), fixes.end(), same) == 1;
        for (const nlohmann::json& fix : fixes){
            if (!single || !same(fix)) continue;
            if (fix.contains("node") && fix["node"].is_string()) problem.nodeId = fix["node"].get<std::string>();
            // a fix of one edit only: more is the agent's to decide, through its fix_hint
            nlohmann::json op;
            if (fix.contains("op") && fix["op"].is_object()) op = fix["op"];
            else if (fix.contains("ops") && fix["ops"].is_array() && fix["ops"].size() == 1 && fix["ops"][0].is_object()) op = fix["ops"][0];
            if (op.is_null()) continue;
            try{
                problem.fix = {graph::opFromJson(GraphJson::parse(op.dump()))};
                problem.fixLabel = fix.value("label", "Fix " + problem.code);
            }catch (const std::exception&){
                problem.fix.clear();
            }
        }
        out.push_back(std::move(problem));
    }
    return out;
}

void ProceduralMaterialWindow::collectProblems(std::vector<Problem>& out) const{
    // why the material has no image (nothing on the Material Output, a value the planner refuses, the device)
    if (material && !material->error().empty()) out.push_back({graph::Severity::Warning, "preview", material->error(), "", {}, ""});
    // R9: a device failure the evaluation continued past on the CPU, with the pass, the device and the log
    if (material && material->hasImage()){
        for (const std::string& warning : material->latest().warnings) out.push_back({graph::Severity::Warning, "device", warning, "", {}, ""});
    }
    const Target target = shownTarget();
    if (focus && !target.node.empty() && !focus->error().empty()){
        out.push_back({graph::Severity::Warning, "preview", "The preview of " + target.node + ": " + focus->error(), target.node, {}, ""});
    }
    for (const Problem& problem : reportProblems) out.push_back(problem);
}

// ---------------------------------------------------------------- thumbnails

void ProceduralMaterialWindow::computeKeys(){
    nodeKeys.clear();
    if (!getDocument()) return;
    const graph::Graph& g = getDocument()->graph();
    std::map<std::string, std::vector<const graph::Edge*>> into;
    for (const graph::Edge& edge : g.edges) into[edge.to.node].push_back(&edge);
    for (const graph::Node& node : g.nodes){
        // what feeds it, values included; positions and the seed are not (a request adds its seed)
        std::set<std::string> seen;
        std::vector<std::string> pending = {node.id};
        while (!pending.empty()){
            const std::string id = pending.back();
            pending.pop_back();
            if (!seen.insert(id).second) continue;
            for (const graph::Edge* edge : into[id]) pending.push_back(edge->from.node);
        }
        uint64_t key = 1469598103934665603ull;
        for (const std::string& id : seen){
            const graph::Node* upstream = g.findNode(id);
            if (!upstream) continue;
            key = mixKey(key, id);
            key = mixKey(key, upstream->type);
            key = mixKey(key, upstream->params.dump());
            key = mixKey(key, upstream->passthrough.dump());
            for (const graph::Edge* edge : into[id]) key = mixKey(key, edge->from.node + "." + edge->from.port + ">" + edge->to.port);
        }
        nodeKeys[node.id] = key;
    }
}

void ProceduralMaterialWindow::buildRequests(){
    requests.clear();
    if (!getDocument()) return;
    const graph::Graph& g = getDocument()->graph();
    const double s = graphSeed();
    const procmat::EvaluatorTier t = budgetTier();
    std::set<std::string> added;
    const auto add = [&](const std::string& id, int priority){
        if (!added.insert(id).second) return;
        const graph::Node* node = g.findNode(id);
        if (!node) return;
        const std::optional<procmat::PlanRoot> root = thumbnailRoot(*node);
        if (!root) return;
        procmat::ProcMatThumbnails::Request request;
        request.id = id;
        request.roots = {*root};
        request.seed = s;
        request.key = mixKey(nodeKeys[id], std::to_string((int64_t)s) + "|" + root->port);
        request.priority = priority;
        requests.push_back(std::move(request));
    };
    // R24: the selection everywhere; the outputs on the device tiers; every visible node on a GPU
    if (!selectedNodes().empty()) add(selectedNodes().front(), 30);
    const graph::Node* output = nullptr;
    for (const graph::Node& node : g.nodes){
        if (node.type == procmat::MATERIAL_OUTPUT_NODE) output = &node;
    }
    if (output && t != procmat::EvaluatorTier::Cpu) add(output->id, 20);
    if (variationsOpen && output){
        if (const std::optional<procmat::PlanRoot> root = thumbnailRoot(*output)){
            const std::vector<int> seeds = variationSeeds();
            for (int i = 0; i < VARIATIONS; i++){
                procmat::ProcMatThumbnails::Request request;
                request.id = VARIATION_PREFIX + std::to_string(i);
                request.roots = {*root};
                request.seed = (double)seeds[(size_t)i];
                request.key = mixKey(nodeKeys[output->id], std::to_string(seeds[(size_t)i]));
                request.priority = 10 - i;
                requests.push_back(std::move(request));
            }
        }
    }
    if (t == procmat::EvaluatorTier::Gpu){
        for (const std::string& id : visible) add(id, 0);
    }
}

bool ProceduralMaterialWindow::nodeHasThumbnail(const graph::Node& node) const{
    return thumbnailRoot(node).has_value();
}

ImTextureID ProceduralMaterialWindow::thumbnailTexture(const std::string& id){
    const procmat::ProcMatThumbnails::Image* image = thumbs->image(id);
    if (!image) return ImTextureID{};
    if (image->device) return Backend::getImGuiTexture(image->device.get());
    if (image->pixels.empty()) return ImTextureID{};
    Upload& shown = thumbUploads[id];
    if (shown.serial != image->serial) upload(shown, image->pixels, procmat::ProcMatThumbnails::IMAGE_SIZE, image->serial);
    return shown.valid() ? Backend::getImGuiTexture(shown.texture.getRender()) : ImTextureID{};
}

void ProceduralMaterialWindow::drawNodeThumbnail(const graph::Node& node, ImDrawList* list, ImVec2 min, ImVec2 max){
    nextVisible.push_back(node.id);
    const procmat::ProcMatThumbnails::Image* image = thumbs->image(node.id);
    const ImTextureID texture = thumbnailTexture(node.id);
    list->AddRectFilled(min, max, IM_COL32(24, 24, 24, 255));
    Widgets::addImage(list, texture, min, max);
    if (!image) return;
    UIProbe::rect("procmat.thumb." + node.id, min.x, min.y, max.x, max.y);
    // the previous image, out of date until its new one is drawn (R7)
    if (thumbs->isOutOfDate(node.id)){
        list->AddRectFilled(min, max, IM_COL32(0, 0, 0, 90));
        list->AddText(ImVec2(min.x + 4.0f, min.y + 2.0f), IM_COL32(255, 220, 120, 255), ICON_FA_CLOCK);
    }
    if (!image->error.empty()) list->AddText(ImVec2(min.x + 4.0f, max.y - ImGui::GetFontSize() - 2.0f), IM_COL32(255, 110, 110, 255), ICON_FA_TRIANGLE_EXCLAMATION);
}

std::string ProceduralMaterialWindow::nodeCost(const graph::Node& node) const{
    const procmat::ProcMatThumbnails::Image* image = thumbs->image(node.id);
    if (!image) return std::string();
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f ms", image->costMs);
    return text;
}

// ---------------------------------------------------------------- images on the GPU

void ProceduralMaterialWindow::retire(Upload& target){
    if (!target.texture.empty()) retired.push_back(target.texture);
    target = Upload();
}

void ProceduralMaterialWindow::upload(Upload& target, const std::vector<uint8_t>& rgba, int size, uint64_t serial){
    retire(target);
    target.serial = serial;
    // no view, no textures (the editor's unit tests): the serial still says it was seen
    if (!Engine::isViewLoaded() || rgba.size() != (size_t)size * (size_t)size * 4) return;
    static uint64_t counter = 0;
    void* pixels = std::malloc(rgba.size());
    if (!pixels) return;
    std::memcpy(pixels, rgba.data(), rgba.size());
    // TextureData frees what it is given
    TextureData data(size, size, (unsigned int)rgba.size(), ColorFormat::RGBA, 4, pixels);
    target.texture.setData("editor:procedural-material:" + std::to_string(++counter), data);
    target.texture.setMinFilter(TextureFilter::LINEAR);
    target.texture.setMagFilter(TextureFilter::LINEAR);
    TextureRender* render = target.texture.getRender();
    target.texture.releaseData();
    if (!render || !render->isCreated()) retire(target);
    target.serial = serial;
}

void ProceduralMaterialWindow::uploadImages(){
    if (material && material->hasImage()){
        const auto& outputs = material->latest().outputs;
        // an output the material no longer has (a disconnected Emission) leaves the ball too
        for (auto it = outputUploads.begin(); it != outputUploads.end();){
            const bool kept = std::any_of(outputs.begin(), outputs.end(), [&](const procmat::OutputImage& image){ return image.name == it->first; });
            if (kept){
                ++it;
            }else{
                retire(it->second);
                it = outputUploads.erase(it);
            }
        }
        for (const procmat::OutputImage& image : outputs){
            Upload& shown = outputUploads[image.name];
            if (shown.serial != materialSerial) upload(shown, image.rgba, image.width, materialSerial);
        }
    }
    if (focus && focus->hasImage() && !focus->latest().outputs.empty() && focusUpload.serial != focusSerial){
        const procmat::OutputImage& image = focus->latest().outputs.front();
        upload(focusUpload, image.rgba, image.width, focusSerial);
    }
}

void ProceduralMaterialWindow::updateBall(){
    if (!Engine::isViewLoaded()) return;
    if (!ball){
        ball = std::make_unique<MaterialRender>();
        ballMaterial = std::make_unique<Material>();
    }
    Material next;
    next.metallicFactor = 0.0f;
    next.roughnessFactor = 1.0f;
    const auto texture = [&](const char* name) -> const Upload* {
        auto it = outputUploads.find(name);
        return it != outputUploads.end() && it->second.valid() ? &it->second : nullptr;
    };
    if (const Upload* base = texture("base_color")) next.baseColorTexture = base->texture;
    if (const Upload* normal = texture("normal")) next.normalTexture = normal->texture;
    if (const Upload* orm = texture("orm")){
        // R14's ORM: occlusion, roughness, metallic in one image, as glTF reads it
        next.occlusionTexture = orm->texture;
        next.metallicRoughnessTexture = orm->texture;
        next.metallicFactor = 1.0f;
    }
    if (const Upload* emission = texture("emission")){
        next.emissiveTexture = emission->texture;
        next.emissiveFactor = Vector3(1.0f, 1.0f, 1.0f);
    }
    if (next != *ballMaterial || ball->isPending() || !ball->getFramebuffer()->isCreated()){
        *ballMaterial = next;
        ball->applyMaterial(next);
        Engine::executeSceneOnce(ball->getScene());
    }
}

// ---------------------------------------------------------------- the frame

void ProceduralMaterialWindow::frameStarted(){
    if (!material) documentChanged();
    // the nodes whose image slot the canvas drew last frame: tier 1 draws those
    visible = std::move(nextVisible);
    nextVisible.clear();
    buildRequests();
    thumbs->setRequests(requests);
    uploadImages();
    updateBall();
    // a debounce or a drawing waits for frames an idle editor would not draw
    const auto waits = [](const procmat::ProceduralSession* session){
        return session && (session->isRebuildPending() || (session->isStale() && session->error().empty()));
    };
    const bool waiting = workLeft || waits(material.get()) || (!shownTarget().node.empty() && waits(focus.get()));
    if (waiting && frameRequester()) frameRequester()();
}

void ProceduralMaterialWindow::drawToolbarExtras(){
    const bool locked = seedLocked();
    ImGui::SameLine();
    ImGui::TextUnformatted("Seed");
    ImGui::SameLine();
    int value = seed();
    ImGui::SetNextItemWidth(Theme::dpi(96.0f));
    ImGui::BeginDisabled(locked);
    if (ImGui::InputInt("##procmat_seed", &value, 1, 100, ImGuiInputTextFlags_EnterReturnsTrue)) setSeed(value);
    ImGui::EndDisabled();
    UIProbe::item("procmat.seed");
    ImGui::SetItemTooltip("The graph's seed: every node's random values derive from it (with each node's own seed)");
    ImGui::SameLine();
    if (ImGui::Button(locked ? ICON_FA_LOCK "##procmat_lock" : ICON_FA_LOCK_OPEN "##procmat_lock")) setSeedLocked(!locked);
    UIProbe::item("procmat.seed.lock");
    ImGui::SetItemTooltip("%s", locked ? "Unlock the seed" : "Lock the seed: Reroll and Variations leave it alone");
    ImGui::SameLine();
    ImGui::BeginDisabled(locked);
    if (ImGui::Button(ICON_FA_DICE " Reroll")) rerollSeed();
    UIProbe::item("procmat.seed.reroll");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("A new seed (one undo step)");
    ImGui::SameLine();
    const bool open = variationsOpen;
    if (open) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_FA_TABLE_CELLS_LARGE " Variations")) variationsOpen = !variationsOpen;
    if (open) ImGui::PopStyleColor();
    UIProbe::item("procmat.variations");
    ImGui::SetItemTooltip("The material at 9 other seeds: click one to use its seed");
}

void ProceduralMaterialWindow::drawImage(ImTextureID texture, const ImVec2& size, bool outOfDate, int tiles, const char* probe){
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    if (probe) UIProbe::item(probe);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + size.x, at.y + size.y);
    list->AddRectFilled(at, end, IM_COL32(24, 24, 24, 255));
    // the tile as copies side by side: a sampler that repeats is the backend's to give, a quad each is not
    const float w = size.x / (float)tiles;
    const float h = size.y / (float)tiles;
    for (int y = 0; y < tiles; y++){
        for (int x = 0; x < tiles; x++){
            Widgets::addImage(list, texture, ImVec2(at.x + w * (float)x, at.y + h * (float)y), ImVec2(at.x + w * (float)(x + 1), at.y + h * (float)(y + 1)));
        }
    }
    if (outOfDate){
        list->AddRectFilled(at, end, IM_COL32(0, 0, 0, 70));
        list->AddText(ImVec2(at.x + 4.0f, at.y + 2.0f), IM_COL32(255, 220, 120, 255), size.x > Theme::dpi(100.0f) ? ICON_FA_CLOCK " Out of date" : ICON_FA_CLOCK);
    }
}

void ProceduralMaterialWindow::drawDevice(){
    const std::string label = tierLabel();
    const std::string name = deviceName();
    ImGui::TextDisabled("Device: %s", tierKnown() ? label.c_str() : "tier unknown");
    UIProbe::item("procmat.tier." + label);
    if (!name.empty()){
        // a renderer's name is long ("llvmpipe (LLVM 15.0.7, 256 bits)"): its own wrapped line
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", name.c_str());
        ImGui::PopTextWrapPos();
    }
    const std::string why = tierReason();
    if (!why.empty()){
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Theme::Colors::WarningText, "%s", why.c_str());
        ImGui::PopTextWrapPos();
        UIProbe::item("procmat.tier.reason");
    }
}

void ProceduralMaterialWindow::drawVariations(){
    const std::vector<int> seeds = variationSeeds();
    const bool locked = seedLocked();
    ImGui::SeparatorText("Variations");
    if (locked) ImGui::TextDisabled("The seed is locked.");
    const float width = ImGui::GetContentRegionAvail().x;
    const float cell = std::floor((width - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f);
    for (int i = 0; i < VARIATIONS; i++){
        if (i % 3 != 0) ImGui::SameLine();
        ImGui::PushID(i);
        const std::string id = VARIATION_PREFIX + std::to_string(i);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool current = isVariationCurrent((size_t)i);
        ImGui::BeginDisabled(locked || !current);
        const bool clicked = ImGui::InvisibleButton("##variation", ImVec2(cell, cell));
        ImGui::EndDisabled();
        UIProbe::item("procmat.variation." + std::to_string(i));
        if (current) UIProbe::item("procmat.variation." + std::to_string(i) + ".current");
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->AddRectFilled(at, ImVec2(at.x + cell, at.y + cell), IM_COL32(24, 24, 24, 255));
        Widgets::addImage(list, thumbnailTexture(id), at, ImVec2(at.x + cell, at.y + cell));
        if (!current){
            list->AddRectFilled(at, ImVec2(at.x + cell, at.y + cell), IM_COL32(0, 0, 0, 110));
            list->AddText(ImVec2(at.x + 4.0f, at.y + 2.0f), IM_COL32(255, 220, 120, 255), ICON_FA_CLOCK);
        }
        if (ImGui::IsItemHovered()) list->AddRect(at, ImVec2(at.x + cell, at.y + cell), IM_COL32(255, 255, 255, 160));
        ImGui::SetItemTooltip("Seed %d: click to use it", seeds[(size_t)i]);
        if (clicked) adoptVariation((size_t)i);
        ImGui::PopID();
    }
}

void ProceduralMaterialWindow::drawExposed(){
    ImGui::SeparatorText("Exposed Properties");
    const std::vector<Exposed> exposed = exposedProperties();
    if (exposed.empty()){
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Right-click a parameter in the Inspector and pick Expose to keep it here.");
        ImGui::PopTextWrapPos();
        return;
    }
    // by node, in the order they were exposed
    std::vector<std::string> order;
    for (const Exposed& e : exposed){
        if (std::find(order.begin(), order.end(), e.node) == order.end()) order.push_back(e.node);
    }
    for (const std::string& id : order){
        const graph::Node* node = getDocument()->graph().findNode(id);
        const graph::NodeTypeDesc* type = getDocument()->nodeType(node->type);
        std::vector<graph::ParamDesc> params;
        for (const Exposed& e : exposed){
            if (e.node != id) continue;
            for (const graph::ParamDesc& p : type->params){
                if (p.name == e.param) params.push_back(p);
            }
        }
        ImGui::TextDisabled("%s (%s)", typeLabelOf(*getDocument(), *node).c_str(), id.c_str());
        ImGui::PushID(id.c_str());
        drawParamRows(*node, params, "##procmat_exposed", "procmat.exposed." + id);
        ImGui::PopID();
    }
}

void ProceduralMaterialWindow::drawPresets(){
    ImGui::SeparatorText("Presets");
    const std::vector<std::string> names = presetNames();
    for (size_t i = 0; i < names.size(); i++){
        ImGui::PushID((int)i);
        if (ImGui::SmallButton(ICON_FA_XMARK)) deletePreset(names[i]);
        ImGui::SetItemTooltip("Delete this preset");
        ImGui::SameLine();
        if (ImGui::Selectable(names[i].c_str())) applyPreset(names[i]);
        UIProbe::item("procmat.preset." + std::to_string(i));
        ImGui::SetItemTooltip("Set the exposed properties to this preset's values (one undo step)");
        ImGui::PopID();
    }
    static char name[64] = "";
    ImGui::BeginDisabled(exposedProperties().empty());
    ImGui::SetNextItemWidth(-Theme::dpi(64.0f));
    ImGui::InputTextWithHint("##procmat_preset_name", "Preset name", name, sizeof(name));
    UIProbe::item("procmat.preset.name");
    ImGui::SameLine();
    if (ImGui::Button("Save") && name[0] != '\0'){
        if (savePreset(name)) name[0] = '\0';
    }
    UIProbe::item("procmat.preset.save");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Keep the exposed properties' values under this name");
}

void ProceduralMaterialWindow::drawBake(){
    // the bake: its size, and the estimate on the slower tiers (the device line above says why)
    static const int SIZES[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192};
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    if (ImGui::BeginCombo("##procmat_bake_size", (std::to_string(bakeSizeValue) + "\xC2\xB2").c_str())){
        for (int size : SIZES){
            if (ImGui::Selectable(std::to_string(size).c_str(), size == bakeSizeValue)) bakeSizeValue = size;
        }
        ImGui::EndCombo();
    }
    UIProbe::item("procmat.bake.size");
    ImGui::SameLine();
    std::string label = ICON_FA_FILE_EXPORT " Bake";
    bool estimated = false;
    const double estimate = bakeEstimateSeconds(bakeSizeValue);
    if (budgetTier() != procmat::EvaluatorTier::Gpu && estimate >= 0.0){
        char text[48];
        if (estimate < 60.0) std::snprintf(text, sizeof(text), " (about %.0f s)", std::max(1.0, std::ceil(estimate)));
        else std::snprintf(text, sizeof(text), " (about %.0f min)", std::ceil(estimate / 60.0));
        label += text;
        estimated = true;
    }
    std::string why;
    const bool ready = canBake(&why);
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button((label + "##procmat_bake").c_str())) bake();
    ImGui::EndDisabled();
    UIProbe::item("procmat.bake");
    if (estimated) UIProbe::item("procmat.bake.estimate");
    ImGui::SetItemTooltip("%s", ready ? "Write the outputs as PNGs and a .material beside the graph (one undo step)" : why.c_str());
    if (services.bakeState){
        const BakeState state = services.bakeState();
        if (state.running){
            // a run tells its stage, not a fraction: an indeterminate bar
            ImGui::ProgressBar(state.progress >= 0.0f ? state.progress : -1.0f * (float)ImGui::GetTime(), ImVec2(-Theme::dpi(64.0f), 0.0f),
                               state.message.c_str());
            UIProbe::item("procmat.bake.running");
            ImGui::SameLine();
            if (ImGui::Button("Cancel##procmat_bake_cancel") && services.cancelBake) services.cancelBake();
        }else if (!state.message.empty()){
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", state.message.c_str());
            ImGui::PopTextWrapPos();
            UIProbe::item("procmat.bake.message");
        }
    }
    if (!bakeError.empty()){
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Theme::Colors::WarningText, "%s", bakeError.c_str());
        ImGui::PopTextWrapPos();
    }
}

void ProceduralMaterialWindow::drawPreviewPane(){
    drawDevice();
    drawBake();
    ImGui::Separator();
    // what is shown: the selection's output, or a pinned slot's
    const Target target = shownTarget();
    std::string title = "Material";
    if (!target.node.empty()){
        if (const graph::Node* node = getDocument()->graph().findNode(target.node)) title = typeLabelOf(*getDocument(), *node) + " (" + target.node + ")";
    }
    ImGui::TextUnformatted(title.c_str());
    UIProbe::item("procmat.preview.title");
    for (int slot = 0; slot < PIN_SLOTS; slot++){
        ImGui::SameLine();
        ImGui::PushID(slot);
        const bool filled = !pins[(size_t)slot].node.empty();
        const bool shown = activePin == slot;
        if (shown) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const std::string label = std::string(ICON_FA_THUMBTACK " ") + (slot == 0 ? "A" : "B");
        if (ImGui::SmallButton(label.c_str())) clickPin(slot);
        if (shown) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) clearPin(slot);
        UIProbe::item(std::string("procmat.pin.") + (slot == 0 ? "a" : "b"));
        ImGui::SetItemTooltip("%s", shown ? "Shown: click to follow the selection again (right click clears it)"
                                          : filled ? "Show the output pinned here (right click clears it)" : "Pin the output shown to this slot");
        ImGui::PopID();
    }

    // the grid first when open: it is what was asked for
    if (variationsOpen) drawVariations();

    const float width = ImGui::GetContentRegionAvail().x;
    // the lit ball of the material and, beside it, the output shown as a 2×2 tile
    const float half = std::floor((width - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    ImTextureID ballTexture{};
    if (ball){
        Texture rendered = ball->getTexture();
        ballTexture = Backend::getImGuiTexture(rendered.getRender());
    }
    const ImVec2 ballAt = ImGui::GetCursorScreenPos();
    Widgets::image(ballTexture, ImVec2(half, half), IM_COL32(24, 24, 24, 255));
    UIProbe::item("procmat.preview.ball");
    const bool ballOutOfDate = !ball || ball->isPending() || !material || material->isStale() || !material->hasImage();
    if (ballOutOfDate){
        ImGui::GetWindowDrawList()->AddText(ImVec2(ballAt.x + 4.0f, ballAt.y + 2.0f), IM_COL32(255, 220, 120, 255), ICON_FA_CLOCK);
    }else if (ballTexture != ImTextureID{}){
        UIProbe::item("procmat.preview.ball.current");
    }
    ImGui::SameLine();

    // the output shown, as a 2×2 tile
    ImTextureID shown{};
    bool outOfDate = false;
    if (target.node.empty()){
        auto it = outputUploads.find("base_color");
        if (it != outputUploads.end() && it->second.valid()) shown = Backend::getImGuiTexture(it->second.texture.getRender());
        outOfDate = material && material->isStale();
    }else{
        if (focusUpload.valid()) shown = Backend::getImGuiTexture(focusUpload.texture.getRender());
        outOfDate = focus && (focus->isStale() || focusSerial == 0);
    }
    drawImage(shown, ImVec2(half, half), outOfDate, 2, "procmat.preview.tile");
    // what the tile shows, and whether it is the image of the graph as it is (UI tests wait for it)
    UIProbe::item("procmat.preview.of." + (target.node.empty() ? std::string("material") : target.node));
    if (shown != ImTextureID{} && !outOfDate) UIProbe::item("procmat.preview.current");

    // one image per output of the material
    if (material && material->hasImage()){
        const auto& outputs = material->latest().outputs;
        const float cell = std::floor((width - 4.0f * ImGui::GetStyle().ItemSpacing.x) / 5.0f);
        for (size_t i = 0; i < outputs.size(); i++){
            if (i > 0) ImGui::SameLine();
            ImGui::BeginGroup();
            auto it = outputUploads.find(outputs[i].name);
            const ImTextureID texture = it != outputUploads.end() && it->second.valid() ? Backend::getImGuiTexture(it->second.texture.getRender()) : ImTextureID{};
            drawImage(texture, ImVec2(cell, cell), material->isStale(), 1, nullptr);
            UIProbe::item("procmat.preview.output." + outputs[i].name);
            ImGui::TextDisabled("%s", outputLabel(outputs[i].name));
            ImGui::EndGroup();
        }
    }else if (material && !material->error().empty()){
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", material->error().c_str());
        ImGui::PopTextWrapPos();
    }

    drawExposed();
    drawPresets();
}

void ProceduralMaterialWindow::drawParamMenuExtras(const graph::Node& node, const std::string& param){
    ImGui::Separator();
    const bool exposed = isExposed(node.id, param);
    if (ImGui::MenuItem(exposed ? "Unexpose" : "Expose in Procedural Material")) setExposed(node.id, param, !exposed);
}
