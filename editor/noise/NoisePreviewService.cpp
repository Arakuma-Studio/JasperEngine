// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NoisePreviewService.h"

#include "noise/NoiseGraphRuntime.h"

#include <algorithm>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    uint8_t toByte(float value, float low, float scale) {
        return static_cast<uint8_t>(std::clamp((value - low) * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    // A flat field shows mid grey, so a constant does not read as black
    float scaleOf(float low, float high) {
        return high > low ? 1.0f / (high - low) : 0.0f;
    }

    void range(const std::vector<float>& values, float& low, float& high) {
        low = high = values.empty() ? 0.0f : values[0];
        for (float value : values) {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }

    void greyImage(const std::vector<float>& values, float low, float high, std::vector<uint8_t>& rgba) {
        rgba.resize(values.size() * 4);
        const float scale = scaleOf(low, high);
        for (size_t i = 0; i < values.size(); i++) {
            const uint8_t grey = scale > 0.0f ? toByte(values[i], low, scale) : 128;
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = grey;
            rgba[i * 4 + 3] = 255;
        }
    }

    void twoChannelImage(const std::vector<float>& a, const std::vector<float>& b, std::vector<uint8_t>& rgba) {
        float lowA, highA, lowB, highB;
        range(a, lowA, highA);
        range(b, lowB, highB);
        rgba.resize(a.size() * 4);
        const float scaleA = scaleOf(lowA, highA), scaleB = scaleOf(lowB, highB);
        for (size_t i = 0; i < a.size(); i++) {
            rgba[i * 4] = scaleA > 0.0f ? toByte(a[i], lowA, scaleA) : 128;
            rgba[i * 4 + 1] = scaleB > 0.0f ? toByte(b[i], lowB, scaleB) : 128;
            rgba[i * 4 + 2] = 40;
            rgba[i * 4 + 3] = 255;
        }
    }

    noise::GridDomain domainOf(const NoisePreviewRequest& request, int size, bool world) {
        noise::GridDomain domain;
        domain.width = domain.depth = size;
        domain.unitsPerTexel = (world ? static_cast<double>(request.terrainSize) : 1.0) / static_cast<double>(size);
        return domain;
    }

}

NoisePreviewService::~NoisePreviewService() {
    cancel();
}

void NoisePreviewService::cancel() {
    currentCancel.cancel();
    task.cancel();
    task = jobs::Task<NoisePreviewResult>();
}

void NoisePreviewService::request(NoisePreviewRequest request) {
    cancel();
    currentCancel = jobs::CancelToken();
    currentRevision = request.revision;
    submitted++;
    jobs::SubmitOptions options;
    // Interactive work, bounded by the thumbnail and image sizes: Lane::Normal, ahead of Background's Heavy
    // compiles and cooks (the spec's Deviations). Each request cancels the one before, so no backlog builds.
    options.lane = jobs::Lane::Normal;
    options.name = "noise-graph-preview";
    jobs::CancelToken token = currentCancel;
    std::shared_ptr<std::atomic<uint64_t>> runs = bodiesRun;
    task = jobs::submit([request = std::move(request), token, runs]() mutable {
        NoisePreviewResult result;
        if (token.isCancelled()) return result;
        runs->fetch_add(1, std::memory_order_relaxed);
        evaluate(request, &token, result);
        return result;
    }, options);
}

bool NoisePreviewService::poll(NoisePreviewResult& out) {
    if (!task.isValid() || !task.isReady()) return false;
    const bool completed = task.status() == jobs::TaskStatus::Completed;
    if (completed) out = std::move(task.get());
    task = jobs::Task<NoisePreviewResult>();
    return completed && !currentCancel.isCancelled();
}

bool NoisePreviewService::evaluate(const NoisePreviewRequest& request, jobs::CancelToken* cancel, NoisePreviewResult& out) {
    out = NoisePreviewResult();
    out.revision = request.revision;
    out.thumbnailSize = request.thumbnailSize;

    noise::CompileOptions options;
    options.previewNodes = true;
    options.maxHeight = request.maxHeight;
    const noise::CompileResult compiled = noise::compile(request.graph, options);
    if (!compiled.graph) return false;
    const noise::CompiledGraph& plan = *compiled.graph;

    bool world = false;
    auto meta = request.graph.metadata.find("noise");
    if (meta != request.graph.metadata.end() && meta->is_object()) world = meta->value("space", std::string("terrain")) == "world";

    noise::NodeProfile profile;
    noise::EvalContext context;
    context.seed = request.seed;
    context.maxHeight = request.maxHeight;
    context.metresPerTexel = static_cast<double>(request.terrainSize) / static_cast<double>(request.thumbnailSize);
    context.variables = request.variables;
    context.cancel = cancel;
    context.parallel = false;       // one Background job: a preview never takes the workers from the frame
    context.profile = &profile;
    const noise::GridDomain thumbnails = domainOf(request, request.thumbnailSize, world);
    context.uvPerUnit = 1.0 / (thumbnails.unitsPerTexel * static_cast<double>(request.thumbnailSize));

    const std::vector<noise::OutputInfo>& outputs = plan.outputs();
    const size_t texels = static_cast<size_t>(request.thumbnailSize) * static_cast<size_t>(request.thumbnailSize);
    std::vector<std::vector<float>> values(outputs.size(), std::vector<float>(texels));
    std::vector<int> indices;
    std::vector<float*> targets;
    for (size_t i = 0; i < outputs.size(); i++) {
        indices.push_back(static_cast<int>(i));
        targets.push_back(values[i].data());
    }
    if (!plan.evaluateGridOutputs(thumbnails, context, indices, targets)) return false;

    for (const noise::CompiledNodeInfo& info : plan.nodes()) {
        for (size_t i = 0; i < outputs.size(); i++) {
            if (outputs[i].nodeId != info.id) continue;
            NoiseNodePreview node;
            node.nodeId = info.id;
            node.outputName = outputs[i].name;
            node.analytic = plan.rangeOf(static_cast<int>(i));
            range(values[i], node.observed.min, node.observed.max);
            // a coord's channels are its first two previews, ":x" then ":z"
            const std::string& name = outputs[i].name;
            const bool coordX = name.size() > 2 && name.compare(name.size() - 2, 2, ":x") == 0 && i + 1 < outputs.size() &&
                                outputs[i + 1].nodeId == info.id && outputs[i + 1].name.size() > 2 &&
                                outputs[i + 1].name.compare(outputs[i + 1].name.size() - 2, 2, ":z") == 0;
            if (coordX) {
                node.twoChannels = true;
                twoChannelImage(values[i], values[i + 1], node.rgba);
            } else {
                greyImage(values[i], node.observed.min, node.observed.max, node.rgba);
            }
            for (const noise::NodeProfileEntry& entry : profile.nodes) {
                if (entry.nodeId == info.id) node.ms = entry.ms;
            }
            out.nodes.push_back(std::move(node));
            break;      // R56: a node shows its first output
        }
    }

    // The Preview image: the height output when there is one, else the graph's first real output
    int imageOutput = plan.outputIndex("height");
    for (size_t i = 0; imageOutput < 0 && i < outputs.size(); i++) {
        if (outputs[i].kind != noise::OutputKind::NodePreview) imageOutput = static_cast<int>(i);
    }
    if (imageOutput >= 0 && request.imageSize > 0) {
        const noise::GridDomain imageDomain = domainOf(request, request.imageSize, world);
        noise::EvalContext imageContext = context;
        imageContext.profile = nullptr;
        imageContext.metresPerTexel = static_cast<double>(request.terrainSize) / static_cast<double>(request.imageSize);
        imageContext.uvPerUnit = 1.0 / (imageDomain.unitsPerTexel * static_cast<double>(request.imageSize));
        std::vector<float> image(static_cast<size_t>(request.imageSize) * static_cast<size_t>(request.imageSize));
        if (!plan.evaluateGrid(imageDomain, imageContext, imageOutput, image.data())) return false;
        out.image.size = request.imageSize;
        out.image.outputName = outputs[static_cast<size_t>(imageOutput)].name;
        range(image, out.image.min, out.image.max);
        double sum = 0.0;
        for (float value : image) sum += value;
        out.image.mean = image.empty() ? 0.0f : static_cast<float>(sum / static_cast<double>(image.size()));
        greyImage(image, out.image.min, out.image.max, out.image.rgba);
    }
    out.ok = true;
    return true;
}
