// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProcMatThumbnails.h"

#include <algorithm>
#include <chrono>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

ProcMatThumbnails::ProcMatThumbnails(std::shared_ptr<const NodeLibrary> library, MakeEvaluator make, TakeImage take)
    : library(std::move(library)), make(std::move(make)), take(std::move(take)) {}

ProcMatThumbnails::~ProcMatThumbnails() = default;

void ProcMatThumbnails::setGraph(const graph::Graph& graph) {
    current = graph;
}

void ProcMatThumbnails::setRequests(std::vector<Request> requests) {
    std::stable_sort(requests.begin(), requests.end(), [](const Request& a, const Request& b) { return a.priority > b.priority; });
    wanted = std::move(requests);
    clock++;
    for (const Request& request : wanted) {
        if (auto it = images.find(request.id); it != images.end()) it->second.used = clock;
    }
    if (drawing.empty()) return;
    const auto still = std::find_if(wanted.begin(), wanted.end(), [&](const Request& r) { return r.id == drawing && r.key == drawingKey; });
    if (still == wanted.end() && device) {
        // what it draws is wanted no longer, or is out of date already: its job stops (R24's tier 3)
        device->cancel();
        EvaluationResult dropped;
        device->advance(0.0, dropped);
        drawing.clear();
        // what it held was never finished: the next image prepares again
        held.reset();
    }
}

bool ProcMatThumbnails::start(const Request& request) {
    if (!current) return false;
    PlanOptions options;
    options.size = IMAGE_SIZE;
    options.readBack = false;
    options.seed = request.seed;
    options.maxBufferSize = IMAGE_SIZE;
    PassPlan plan;
    std::string error;
    Entry& entry = images[request.id];
    entry.used = clock;
    if (!planGraph(*current, library, request.roots, options, plan, error)) {
        // no image for this key: asked again only once it changes
        entry.image.key = request.key;
        entry.image.error = error;
        return false;
    }
    if (!device) device = make();
    if (!device) return false;
    if (held && plan.sameStructure(*held)) {
        // the image the evaluator holds with other values (a slider on the selected node): no compile
        bool edited = false;
        for (size_t i = 0; i < plan.passes.size(); i++) {
            if (plan.passes[i].uniforms == held->passes[i].uniforms) continue;
            device->setUniforms((uint32_t)i, plan.passes[i].uniforms);
            edited = true;
        }
        if (!edited) {
            // the same image again (another id asks for what is drawn): prepare it anew
            if (!device->prepare(plan, error)) {
                entry.image.key = request.key;
                entry.image.error = error;
                held.reset();
                return false;
            }
            prepares++;
        }
    } else {
        if (!device->prepare(plan, error)) {
            entry.image.key = request.key;
            entry.image.error = error;
            held.reset();
            return false;
        }
        prepares++;
    }
    held = std::move(plan);
    drawing = request.id;
    drawingKey = request.key;
    drawingOutput = held->outputs.empty() ? std::string() : held->outputs.front().name;
    drawingMs = 0.0;
    return true;
}

void ProcMatThumbnails::finish(EvaluationResult& result) {
    const std::string id = drawing;
    drawing.clear();
    Entry& entry = images[id];
    entry.used = clock;
    entry.image.key = drawingKey;
    entry.image.costMs = drawingMs;
    finished++;
    if (!result.ok) {
        entry.image.error = result.error;
        held.reset();
        return;
    }
    entry.image.error.clear();
    std::shared_ptr<TextureRender> fromDevice = take ? take(*device, drawingOutput) : nullptr;
    entry.image.device = std::move(fromDevice);
    entry.image.pixels.clear();
    if (!entry.image.device && !result.outputs.empty()) entry.image.pixels = std::move(result.outputs.front().rgba);
    entry.image.serial = ++serials;
    evict();
}

void ProcMatThumbnails::evict() {
    while (images.size() > CAPACITY) {
        auto oldest = images.end();
        for (auto it = images.begin(); it != images.end(); ++it) {
            if (it->first == drawing) continue;
            if (oldest == images.end() || it->second.used < oldest->second.used) oldest = it;
        }
        if (oldest == images.end()) return;
        images.erase(oldest);
    }
}

bool ProcMatThumbnails::update(double budgetMs) {
    const auto begin = std::chrono::steady_clock::now();
    const auto spent = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(); };
    if (drawing.empty()) {
        // the most wanted request whose image is not of its key (one start per frame)
        for (const Request& request : wanted) {
            auto it = images.find(request.id);
            if (it != images.end() && it->second.image.key == request.key && (it->second.image.serial != 0 || !it->second.image.error.empty())) continue;
            start(request);
            break;
        }
    }
    if (!drawing.empty() && device) {
        EvaluationResult result;
        const bool done = device->advance(budgetMs, result);
        drawingMs += spent();
        if (done) finish(result);
    }
    if (!drawing.empty()) return true;
    for (const Request& request : wanted) {
        if (isOutOfDate(request.id)) return true;
    }
    return false;
}

void ProcMatThumbnails::clear() {
    if (device && !drawing.empty()) {
        device->cancel();
        EvaluationResult dropped;
        device->advance(0.0, dropped);
    }
    drawing.clear();
    held.reset();
    images.clear();
    wanted.clear();
    current.reset();
    device.reset();
}

const ProcMatThumbnails::Image* ProcMatThumbnails::image(const std::string& id) const {
    auto it = images.find(id);
    if (it == images.end() || it->second.image.serial == 0) return nullptr;
    return &it->second.image;
}

bool ProcMatThumbnails::isOutOfDate(const std::string& id) const {
    auto want = std::find_if(wanted.begin(), wanted.end(), [&](const Request& r) { return r.id == id; });
    auto it = images.find(id);
    if (it == images.end()) return true;
    if (want == wanted.end()) return false;
    return it->second.image.key != want->key;
}
