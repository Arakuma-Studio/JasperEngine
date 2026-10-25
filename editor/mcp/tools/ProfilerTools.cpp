// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Profiling and render diagnostics tools (spec S15 §6.6, docs/mcp/guide/profiling.md): the
// render counters and GPU time of a view, the frame-time history with spikes and monitors,
// profiler captures (.jetrace.json) and their CPU scope trees, the frame's pass list with GPU
// pass times, a pass's output as an image, and the VRAM report. All read-only and allowed while
// playing. Measurement caveats are in each description: the first frame after arming is partial,
// armed timings include the profiler's overhead, a frame is one sample, and GPU semantics differ
// per backend.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "tools/PlayViewCommon.h"

#include "App.h"
#include "Project.h"
#include "render/RenderUtil.h"
#include "render/SystemRender.h"
#include "profiler/FrameTimes.h"
#include "profiler/GpuMemory.h"
#include "profiler/Profiler.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

constexpr uint32_t FRAME_TAG = SystemRender::FRAME_STATS_FRAME;

// ------------------------------------------------------------------ views

// The view a tool reads: omitted, the frame the engine draws (the running game while playing,
// else the Scene view); "game" and "scene" by name; an integer is a raw view tag
bool resolveView(const CallPtr& call, uint32_t& tag, std::string& name){
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const bool playing = project && project->isPlaySessionActive();
    if (!args.contains("view") || args["view"].is_null()){
        tag = FRAME_TAG;
        name = playing ? "game" : "scene";
        return true;
    }
    const Json& view = args["view"];
    if (view.is_number_integer()){
        const long long value = view.get<long long>();
        tag = value < 0 ? FRAME_TAG : (uint32_t)value;
        name = std::to_string(tag == FRAME_TAG ? -1 : (long long)tag);
        return true;
    }
    if (view == "game"){
        tag = playing ? FRAME_TAG : GAME_VIEW_ID;
        name = "game";
        return true;
    }
    if (view == "scene"){
        tag = playing ? SCENE_VIEW_ID : FRAME_TAG;
        name = "scene";
        return true;
    }
    call->fail("view must be \"game\", \"scene\" or a view tag (integer; -1 is the engine's frame).");
    return false;
}

Json viewSchema(){
    Json value = {{"type", Json::array({"string", "integer"})},
                  {"description", "game|scene|tag. Omitted: the engine's frame (game while playing, else scene)."}};
    return value;
}

// ------------------------------------------------------------------ statistics

double percentile(std::vector<double> values, double p){
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double position = p * (double)(values.size() - 1);
    const size_t low = (size_t)std::floor(position);
    const size_t high = std::min(low + 1, values.size() - 1);
    const double t = position - (double)low;
    return values[low] * (1.0 - t) + values[high] * t;
}

double round3(double value){
    return std::round(value * 1000.0) / 1000.0;
}

Json summaryOf(const std::vector<double>& values){
    if (values.empty())
        return nullptr;
    return Json{{"median", round3(percentile(values, 0.5))}, {"p95", round3(percentile(values, 0.95))},
                {"p99", round3(percentile(values, 0.99))}, {"max", round3(*std::max_element(values.begin(), values.end()))}};
}

std::string lower(std::string text){
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return text;
}

bool matches(const std::string& text, const std::string& filter){
    return filter.empty() || lower(text).find(lower(filter)) != std::string::npos;
}

Json gpuStateJson(const GpuTimingState& state){
    Json json = {{"available", state.available}, {"enabled", state.enabled}, {"semantics", state.semantics},
                 {"frame_is_sum", state.frameIsSum}};
    if (!state.available)
        json["reason"] = state.reason;
    if (!state.passReason.empty())
        json["pass_reason"] = state.passReason;
    return json;
}

const char* semanticsNote(const std::string& semantics){
    if (semantics == "encoder_span")
        return "Metal: encoder spans overlap; do not sum pass times.";
    if (semantics == "elapsed")
        return "GL: one elapsed query per pass; the frame time is their sum.";
    return "Timestamps at pass start and end; the frame time is first begin to last end.";
}

// ------------------------------------------------------------------ get_render_stats

Json renderStatsJson(uint32_t tag, const std::string& viewName){
    const RenderStats stats = SystemRender::getRenderStats(tag);
    const FrameTimeSample frame = FrameTimes::latest();
    const GpuTimingState gpu = SystemRender::getGpuTimingState();
    Json gpuJson = gpuStateJson(gpu);
    gpuJson["frame_ms"] = stats.gpuFrameValid ? Json(round3(stats.gpuFrameMs)) : Json(nullptr);
    return Json{
        {"view", viewName},
        {"view_tag", tag == FRAME_TAG ? -1 : (long long)tag},
        {"drawn", stats.valid},
        {"frame", stats.frame},
        {"draw_calls", stats.drawCalls},
        {"instances", stats.instances},
        {"triangles", stats.triangles},
        {"passes", stats.passes},
        {"pipelines_applied", stats.pipelinesApplied},
        {"bindings_applied", stats.bindingsApplied},
        {"uniform_bytes", stats.uniformBytes},
        {"image_upload_bytes", stats.imageUploadBytes},
        {"buffer_upload_bytes", stats.bufferUploadBytes},
        {"alive", {{"images", stats.aliveImages}, {"buffers", stats.aliveBuffers}, {"pipelines", stats.alivePipelines},
                   {"views", stats.aliveViews}, {"shaders", stats.aliveShaders}, {"samplers", stats.aliveSamplers}}},
        {"pipeline_cache", {{"misses", stats.pipelineCacheMisses}, {"applies", stats.pipelineCacheApplies},
                            {"size", stats.pipelineCacheSize}}},
        {"cpu_ms", round3(frame.cpuMs)},
        {"frame_ms", round3(frame.frameMs)},
        {"fps", round3(frame.fps)},
        {"gpu", gpuJson},
    };
}

void getRenderStatsTool(const CallPtr& call){
    uint32_t tag = FRAME_TAG;
    std::string name;
    if (!resolveView(call, tag, name))
        return;
    Json data = renderStatsJson(tag, name);
    std::string text = "Render stats (" + name + "): " + std::to_string(data["draw_calls"].get<uint32_t>()) + " draws, " +
        std::to_string(data["passes"].get<uint32_t>()) + " passes, CPU " + std::to_string(data["cpu_ms"].get<double>()).substr(0, 6) + " ms";
    if (!data["drawn"].get<bool>())
        text += " (this view has not drawn in a recorded frame)";
    call->ok(text + ".", data);
}

// ------------------------------------------------------------------ get_frame_times

void getFrameTimesTool(const CallPtr& call){
    const Json& args = call->args();
    const int window = std::clamp(args.value("window", 300), 1, (int)FrameTimes::CAPACITY);
    const int newest = std::clamp(args.value("frames", 20), 0, (int)FrameTimes::CAPACITY);
    const std::vector<FrameTimeSample> samples = FrameTimes::snapshot((size_t)window);
    std::vector<double> cpu, gpu, fps, frameMs;
    for (const FrameTimeSample& sample : samples){
        cpu.push_back(sample.cpuMs);
        frameMs.push_back(sample.frameMs);
        fps.push_back(sample.fps);
        if (sample.gpuMs >= 0.0f)
            gpu.push_back(sample.gpuMs);
    }
    Json data = {{"frames_in_window", samples.size()},
                 {"summary", {{"cpu_ms", summaryOf(cpu)}, {"gpu_ms", summaryOf(gpu)}, {"fps", summaryOf(fps)},
                              {"frame_ms", summaryOf(frameMs)}}}};
    if (args.contains("min_frame_ms")){
        const double threshold = args["min_frame_ms"].get<double>();
        Json spikes = Json::array();
        std::vector<uint64_t> spikeFrames;
        for (const FrameTimeSample& sample : samples){
            if (sample.frameMs < threshold)
                continue;
            spikeFrames.push_back(sample.frame);
            if (spikes.size() < 100)
                spikes.push_back({{"frame", sample.frame}, {"frame_ms", round3(sample.frameMs)}, {"cpu_ms", round3(sample.cpuMs)},
                                  {"gpu_ms", sample.gpuMs >= 0.0f ? Json(round3(sample.gpuMs)) : Json(nullptr)}});
        }
        std::vector<double> intervals;
        for (size_t i = 1; i < spikeFrames.size(); i++)
            intervals.push_back((double)(spikeFrames[i] - spikeFrames[i - 1]));
        data["spikes"] = spikes;
        data["spike_count"] = spikeFrames.size();
        data["spike_interval_frames"] = intervals.empty() ? Json(nullptr) : Json((long long)std::llround(percentile(intervals, 0.5)));
    }
    if (newest > 0){
        Json list = Json::array();
        for (size_t i = samples.size() > (size_t)newest ? samples.size() - (size_t)newest : 0; i < samples.size(); i++){
            const FrameTimeSample& sample = samples[i];
            list.push_back({{"frame", sample.frame}, {"frame_ms", round3(sample.frameMs)}, {"cpu_ms", round3(sample.cpuMs)},
                            {"gpu_ms", sample.gpuMs >= 0.0f ? Json(round3(sample.gpuMs)) : Json(nullptr)},
                            {"draw_calls", sample.drawCalls}});
        }
        data["newest"] = list;
    }
    if (args.contains("monitors")){
        Json monitors = Json::object();
        for (const Json& item : args["monitors"]){
            const std::string name = item.get<std::string>();
            profiler::MonitorInfo info;
            if (!profiler::monitorInfo(name, info)){
                std::string known;
                for (const profiler::MonitorInfo& monitor : profiler::monitors())
                    known += (known.empty() ? "" : ", ") + monitor.name;
                call->fail("No monitor \"" + name + "\". Monitors: " + known + ".");
                return;
            }
            Json series = Json::array();
            for (double value : profiler::monitorSeries(name, (size_t)window))
                series.push_back(round3(value));
            monitors[name] = {{"unit", profiler::monitorUnitName(info.unit)}, {"latest", round3(info.latest)},
                              {"average", round3(info.average)}, {"min", round3(info.minimum)}, {"max", round3(info.maximum)},
                              {"series", series}};
        }
        data["monitors"] = monitors;
    }
    std::string text = std::to_string(samples.size()) + " frames";
    if (!frameMs.empty())
        text += ", median " + std::to_string(round3(percentile(frameMs, 0.5))).substr(0, 6) + " ms";
    if (data.contains("spike_count"))
        text += ", " + std::to_string(data["spike_count"].get<size_t>()) + " spikes";
    call->ok(text + ".", data);
}

// ------------------------------------------------------------------ profiler_capture

Json categoriesJson(const std::vector<profiler::Frame>& frames){
    Json categories = Json::object();
    for (size_t c = 0; c < profiler::CATEGORY_COUNT; c++){
        std::vector<double> values;
        for (const profiler::Frame& frame : frames)
            values.push_back(frame.categoryMs[c]);
        const double median = percentile(values, 0.5);
        if (median > 0.0005 || percentile(values, 0.95) > 0.0005)
            categories[profiler::categoryName((profiler::Category)c)] = {{"median_ms", round3(median)}, {"p95_ms", round3(percentile(values, 0.95))}};
    }
    return categories;
}

std::string resolveSavePath(const CallPtr& call, const std::string& path){
    std::filesystem::path out(path);
    if (out.is_relative()){
        Project* project = call->ctx().project();
        if (project)
            out = project->getProjectPath() / out;
    }
    return out.lexically_normal().string();
}

void profilerCaptureTool(const CallPtr& call){
    const Json& args = call->args();
    const uint32_t frames = (uint32_t)std::clamp(args.value("frames", 120), 1, 600);
    const bool gpu = args.value("gpu", true);
    const bool deepLua = args.value("deep_lua", false);
    const std::string savePath = args.contains("save_path") ? resolveSavePath(call, args["save_path"].get<std::string>()) : std::string();
    if (profiler::activeCapture()){
        call->fail("A capture is already running (the Profiler window or another call); wait for it.");
        return;
    }
    const bool previousDeep = profiler::isDeepLua();
    profiler::setDeepLua(deepLua);
    if (gpu){
        SystemRender::acquireGpuTiming();
        profiler::setGpuSemantics(SystemRender::getGpuTimingState().semantics);
    }
    const profiler::CaptureId id = profiler::startCapture(frames);
    if (!id){
        profiler::setDeepLua(previousDeep);
        if (gpu)
            SystemRender::releaseGpuTiming();
        call->fail("The capture could not start.");
        return;
    }
    App& app = call->ctx().app();
    app.requestContinuousFrames(1.0);
    const int timeoutMs = (int)std::clamp(frames * 250.0, 30000.0, 300000.0);
    call->waitUntil([id, &app](){
        app.requestContinuousFrames(1.0);
        profiler::CaptureInfo info;
        return !profiler::captureInfo(id, info) || info.complete;
    }, timeoutMs, [call, id, gpu, previousDeep, savePath, frames](bool timedOut){
        profiler::setDeepLua(previousDeep);
        if (gpu)
            SystemRender::releaseGpuTiming();
        profiler::CaptureInfo info;
        if (timedOut || !profiler::captureInfo(id, info) || !info.complete){
            const uint32_t recorded = profiler::captureInfo(id, info) ? info.framesRecorded : 0;
            profiler::cancelCapture();
            call->fail("Only " + std::to_string(recorded) + " of " + std::to_string(frames) +
                       " frames were drawn in time (a minimized or idle editor draws none).");
            return;
        }
        const std::vector<profiler::Frame>& captured = *profiler::captureFrames(id);
        std::vector<double> frameMs, cpuMs, gpuMs;
        uint64_t events = 0, dropped = 0;
        size_t worst = 0;
        for (size_t i = 0; i < captured.size(); i++){
            frameMs.push_back(captured[i].frameMs());
            cpuMs.push_back(captured[i].cpuMs);
            if (captured[i].gpuMs >= 0.0)
                gpuMs.push_back(captured[i].gpuMs);
            events += captured[i].events;
            dropped += captured[i].dropped;
            if (captured[i].frameMs() > captured[worst].frameMs())
                worst = i;
        }
        const double eventsPerFrame = captured.empty() ? 0.0 : (double)events / (double)captured.size();
        const double overheadMs = eventsPerFrame / 2.0 * profiler::scopePairCostNs() / 1.0e6;
        const double medianFrame = percentile(frameMs, 0.5);
        Json data = {
            {"capture_id", id},
            {"frames", captured.size()},
            {"first_frame", captured.empty() ? 0 : captured.front().index},
            {"frame_ms", summaryOf(frameMs)},
            {"cpu_ms", summaryOf(cpuMs)},
            {"gpu_ms", summaryOf(gpuMs)},
            {"categories", categoriesJson(captured)},
            {"worst_frame", captured.empty() ? 0 : captured[worst].index},
            {"worst_frame_ms", captured.empty() ? 0.0 : round3(captured[worst].frameMs())},
            {"events_per_frame", (long long)std::llround(eventsPerFrame)},
            {"dropped_events", dropped},
            {"overhead_note", "Recording adds about " + std::to_string(round3(overheadMs)).substr(0, 5) + " ms a frame (" +
                std::to_string((long long)std::llround(medianFrame > 0.0 ? overheadMs * 100.0 / medianFrame : 0.0)) + "% estimated)."},
            {"gpu_semantics", SystemRender::getGpuTimingState().semantics},
        };
        std::string text = "Captured " + std::to_string(captured.size()) + " frames (capture " + std::to_string(id) + ")";
        if (!savePath.empty()){
            std::string error;
            if (!profiler::saveCapture(id, savePath, &error)){
                call->fail("Captured, but the file was not written: " + error, data);
                return;
            }
            data["path"] = savePath;
            text += ", written to " + savePath;
        }
        call->ok(text + ".", data);
    });
}

// ------------------------------------------------------------------ get_cpu_profile

const profiler::Frame* pickFrame(const CallPtr& call, const std::vector<const profiler::Frame*>& frames){
    const Json& args = call->args();
    if (frames.empty())
        return nullptr;
    if (!args.contains("frame") || args["frame"] == "latest")
        return frames.back();
    const Json& frame = args["frame"];
    if (frame.is_number_integer()){
        const uint64_t index = frame.get<uint64_t>();
        for (const profiler::Frame* candidate : frames){
            if (candidate->index == index)
                return candidate;
        }
        call->fail("Frame " + std::to_string(index) + " is not kept: frames " + std::to_string(frames.front()->index) + " to " +
                   std::to_string(frames.back()->index) + " are.");
        return nullptr;
    }
    std::vector<const profiler::Frame*> sorted = frames;
    std::sort(sorted.begin(), sorted.end(), [](const profiler::Frame* a, const profiler::Frame* b){ return a->frameMs() < b->frameMs(); });
    if (frame == "worst")
        return sorted.back();
    if (frame == "median")
        return sorted[sorted.size() / 2];
    call->fail("frame must be a frame index, \"latest\", \"worst\" or \"median\".");
    return nullptr;
}

void getCpuProfileTool(const CallPtr& call){
    const Json& args = call->args();
    const int maxRows = std::clamp(args.value("max_rows", 64), 1, 1000);
    const std::string view = args.value("view", std::string("tree"));
    const std::string filter = args.value("filter", std::string());
    std::vector<const profiler::Frame*> frames;
    std::vector<profiler::ThreadInfo> threads;
    profiler::NameLookup lookup;
    profiler::CaptureId captureId = 0;
    if (args.contains("capture_id")){
        captureId = args["capture_id"].get<uint32_t>();
        const std::vector<profiler::Frame>* captured = profiler::captureFrames(captureId);
        if (!captured){
            call->fail("No capture " + std::to_string(captureId) + " (profiler_capture returns capture_id; 8 are kept).");
            return;
        }
        for (const profiler::Frame& frame : *captured)
            frames.push_back(&frame);
        threads = profiler::captureThreads(captureId);
        lookup = [captureId](uint32_t nameId){ return profiler::captureNameOf(captureId, nameId); };
    }else{
        for (size_t i = 0; i < profiler::frameCount(); i++){
            const profiler::Frame* frame = profiler::frameAt(i);
            if (frame->hasDetail)
                frames.push_back(frame);
        }
        threads = profiler::threads();
    }
    if (frames.empty()){
        call->fail(captureId ? "The capture has no frames yet." :
                   "The profiler has recorded no frames: run profiler_capture, or turn on Record in Window > Analysis > Profiler.");
        return;
    }
    const profiler::Frame* frame = pickFrame(call, frames);
    if (!frame)
        return;

    uint32_t thread = 0;
    std::string threadName;
    for (const profiler::ThreadInfo& info : threads){
        if (info.main && !thread){
            thread = info.id;
            threadName = info.name;
        }
    }
    if (args.contains("thread")){
        const Json& wanted = args["thread"];
        thread = 0;
        for (const profiler::ThreadInfo& info : threads){
            if ((wanted.is_number_integer() && info.id == wanted.get<uint32_t>()) ||
                (wanted.is_string() && lower(info.name) == lower(wanted.get<std::string>()))){
                thread = info.id;
                threadName = info.name;
            }
        }
        if (!thread){
            std::string known;
            for (const profiler::ThreadInfo& info : threads)
                known += (known.empty() ? "" : ", ") + info.name;
            call->fail("No thread " + wanted.dump() + ". Threads: " + known + ".");
            return;
        }
    }
    auto threadLabel = [&](uint32_t id){
        for (const profiler::ThreadInfo& info : threads){
            if (info.id == id)
                return info.name;
        }
        return "Thread " + std::to_string(id);
    };

    Json data = {{"frame", frame->index}, {"frame_ms", round3(frame->frameMs())}, {"cpu_ms", round3(frame->cpuMs)},
                 {"gpu_ms", frame->gpuMs >= 0.0 ? Json(round3(frame->gpuMs)) : Json(nullptr)}, {"partial", frame->partial},
                 {"dropped_events", frame->dropped}, {"view", view}};
    if (captureId)
        data["capture_id"] = captureId;
    Json rows = Json::array();
    size_t total = 0;
    if (view == "tree"){
        data["thread"] = threadName;
        for (const profiler::TreeRow& row : profiler::buildTree(*frame, thread, lookup)){
            if (!matches(row.name, filter))
                continue;
            total++;
            if ((int)rows.size() < maxRows)
                rows.push_back({{"name", row.name}, {"depth", row.depth}, {"total_ms", round3(row.totalMs)},
                                {"self_ms", round3(row.selfMs)}, {"calls", row.calls}});
        }
    }else if (view == "flat"){
        for (const profiler::FlatRow& row : profiler::buildFlat(*frame, lookup)){
            if (!matches(row.name, filter))
                continue;
            total++;
            if ((int)rows.size() >= maxRows)
                continue;
            Json perThread = Json::object();
            for (const auto& [id, ms] : row.perThread)
                perThread[threadLabel(id)] = round3(ms);
            rows.push_back({{"name", row.name}, {"total_ms", round3(row.totalMs)}, {"self_ms", round3(row.selfMs)},
                            {"calls", row.calls}, {"threads", perThread}});
        }
    }else if (view == "threads"){
        for (const profiler::ThreadBusy& busy : profiler::buildThreadBusy(*frame)){
            total++;
            rows.push_back({{"thread", threadLabel(busy.thread)}, {"busy_ms", round3(busy.busyMs)},
                            {"busy_percent", round3(busy.busyPercent)}, {"scopes", busy.scopes}});
        }
    }else{
        call->fail("view must be \"tree\", \"flat\" or \"threads\".");
        return;
    }
    data["rows"] = rows;
    data["rows_total"] = total;
    call->ok("Frame " + std::to_string(frame->index) + " (" + std::to_string(round3(frame->frameMs())).substr(0, 6) + " ms): " +
             std::to_string(rows.size()) + " of " + std::to_string(total) + " rows.", data);
}

// ------------------------------------------------------------------ get_gpu_passes

const char* loadName(PassLoadAction action){
    return action == PassLoadAction::Load ? "load" : (action == PassLoadAction::DontCare ? "dontcare" : "clear");
}

const char* storeName(PassStoreAction action){
    return action == PassStoreAction::Store ? "store" : "dontcare";
}

Json attachmentJson(const char* slot, const PassAttachment& attachment){
    Json json = {{"attachment", slot}, {"label", attachment.label}, {"width", attachment.width}, {"height", attachment.height},
                 {"format", attachment.format}, {"samples", attachment.samples}};
    if (attachment.mip)
        json["mip"] = attachment.mip;
    if (attachment.cube || attachment.array)
        json["slice"] = attachment.slice;
    return json;
}

Json passJson(const PassRecord& pass){
    Json targets = Json::array();
    Json actions = Json::object();
    Json colors = Json::array();
    for (int i = 0; i < pass.numColors; i++){
        if (pass.colors[i].used){
            targets.push_back(attachmentJson(("color" + std::to_string(i)).c_str(), pass.colors[i]));
            colors.push_back(std::string(loadName(pass.colorLoad[i])) + "/" + storeName(pass.colorStore[i]));
        }
        if (pass.resolves[i].used)
            targets.push_back(attachmentJson(("resolve" + std::to_string(i)).c_str(), pass.resolves[i]));
    }
    if (pass.depth.used){
        targets.push_back(attachmentJson("depth", pass.depth));
        actions["depth"] = std::string(loadName(pass.depthLoad)) + "/" + storeName(pass.depthStore);
    }
    if (pass.swapchain){
        targets.push_back({{"attachment", "swapchain"}, {"width", pass.width}, {"height", pass.height}, {"samples", pass.samples}});
        colors.push_back(std::string(loadName(pass.colorLoad[0])) + "/" + storeName(pass.colorStore[0]));
    }
    actions["color"] = colors;
    Json json = {{"index", pass.index}, {"label", pass.label}, {"targets", targets}, {"actions", actions},
                 {"draws", pass.draws}, {"instances", pass.instances}, {"triangles", pass.triangles},
                 {"pipelines", pass.pipelines}, {"bindings", pass.bindings}, {"uniform_bytes", pass.uniformBytes},
                 {"cpu_ms", round3(pass.cpuMs)}, {"gpu_ms", pass.gpuValid ? Json(std::round(pass.gpuMs * 10000.0) / 10000.0) : Json(nullptr)},
                 {"valid", pass.gpuValid}};
    if (!pass.executed)
        json["skipped"] = true;
    if (pass.compute)
        json["compute"] = true;
    return json;
}

// the capture's passes of a view, aggregated by label over its frames
void capturePassesJson(const CallPtr& call, profiler::CaptureId id, uint32_t tag, Json& data, std::string& text){
    const std::vector<profiler::Frame>* frames = profiler::captureFrames(id);
    struct Aggregate {
        std::vector<double> perFrameMs;
        std::vector<double> perFrameCount;
        size_t order = 0;
    };
    std::map<std::string, Aggregate> passes;
    std::vector<double> frameMs;
    size_t framesWithGpu = 0;
    for (const profiler::Frame& frame : *frames){
        std::map<std::string, std::pair<double, double>> sums;
        bool any = false;
        for (const profiler::GpuPassSample& pass : frame.gpu){
            if (pass.viewTag != tag || !pass.valid)
                continue;
            auto& sum = sums[pass.label];
            sum.first += pass.gpuMs;
            sum.second += 1.0;
            if (!passes.count(pass.label))
                passes[pass.label].order = passes.size();
            any = true;
        }
        if (!any)
            continue;
        framesWithGpu++;
        double total = 0.0;
        for (auto& [label, aggregate] : passes){
            auto found = sums.find(label);
            aggregate.perFrameMs.push_back(found != sums.end() ? found->second.first : 0.0);
            aggregate.perFrameCount.push_back(found != sums.end() ? found->second.second : 0.0);
            if (found != sums.end())
                total += found->second.first;
        }
        if (tag == FRAME_TAG && frame.gpuMs >= 0.0)
            frameMs.push_back(frame.gpuMs);
        else
            frameMs.push_back(total);
    }
    std::vector<std::pair<std::string, const Aggregate*>> ordered;
    for (const auto& [label, aggregate] : passes)
        ordered.emplace_back(label, &aggregate);
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b){ return a.second->order < b.second->order; });
    Json list = Json::array();
    for (const auto& [label, aggregate] : ordered){
        list.push_back({{"label", label}, {"per_frame", round3(percentile(aggregate->perFrameCount, 0.5))},
                        {"gpu_ms", summaryOf(aggregate->perFrameMs)}});
    }
    data["capture_id"] = id;
    data["frames_with_gpu"] = framesWithGpu;
    data["frame_gpu_ms"] = summaryOf(frameMs);
    data["passes"] = list;
    text = std::to_string(list.size()) + " pass labels over " + std::to_string(framesWithGpu) + " frames of capture " + std::to_string(id);
}

void replyPasses(const CallPtr& call, uint32_t tag, const std::string& viewName, bool includeDraws, int maxDraws){
    const GpuTimingState state = SystemRender::getGpuTimingState();
    PassList list = SystemRender::getPassList(tag, state.available);
    if (list.passes.empty()){
        call->fail("The " + viewName + " view has not drawn in a recorded frame (is its tab shown? the Game view draws only while visible).");
        return;
    }
    Json passes = Json::array();
    for (const PassRecord& pass : list.passes)
        passes.push_back(passJson(pass));
    Json data = {{"view", viewName}, {"frame", list.frame}, {"commits", list.commits.size()}, {"passes", passes},
                 {"semantics", state.semantics}, {"semantics_note", semanticsNote(state.semantics)},
                 {"availability", {{"state", state.available ? "available" : "unavailable"}}},
                 {"frame_gpu_ms", list.gpuFrameValid ? Json(round3(list.gpuFrameMs)) : Json(nullptr)}};
    if (!state.available)
        data["availability"]["reason"] = state.reason;
    if (!state.passReason.empty())
        data["availability"]["pass_reason"] = state.passReason;
    if (list.droppedPasses)
        data["dropped_passes"] = list.droppedPasses;
    if (includeDraws){
        Json draws = Json::array();
        for (const PassRecord& pass : list.passes){
            for (uint32_t d = 0; d < pass.numDraws && (int)draws.size() < maxDraws; d++){
                const DrawRecord& draw = list.draws[pass.firstDraw + d];
                draws.push_back({{"pass", pass.index}, {"pipeline", draw.pipeline}, {"shader", draw.shader},
                                 {"elements", draw.elements}, {"instances", draw.instances}, {"base_element", draw.baseElement}});
            }
        }
        data["draws"] = draws;
    }
    double sum = 0.0;
    size_t timed = 0;
    for (const PassRecord& pass : list.passes){
        if (pass.gpuValid){
            sum += pass.gpuMs;
            timed++;
        }
    }
    std::string text = std::to_string(list.passes.size()) + " passes in frame " + std::to_string(list.frame) + " of the " + viewName + " view";
    if (timed)
        text += ", " + std::to_string(timed) + " timed (" + std::to_string(round3(sum)).substr(0, 6) + " ms summed)";
    else if (!state.available)
        text += "; GPU timing unavailable: " + state.reason;
    call->ok(text + ".", data);
}

void getGpuPassesTool(const CallPtr& call){
    const Json& args = call->args();
    uint32_t tag = FRAME_TAG;
    std::string name;
    if (!resolveView(call, tag, name))
        return;
    if (args.contains("capture_id")){
        const profiler::CaptureId id = args["capture_id"].get<uint32_t>();
        if (!profiler::captureFrames(id)){
            call->fail("No capture " + std::to_string(id) + " (profiler_capture returns capture_id).");
            return;
        }
        Json data = {{"view", name}, {"semantics", SystemRender::getGpuTimingState().semantics}};
        std::string text;
        capturePassesJson(call, id, tag, data, text);
        call->ok(text + ".", data);
        return;
    }
    const bool includeDraws = args.value("include_draws", false);
    const int maxDraws = std::clamp(args.value("max_draws", 200), 1, 5000);
    const GpuTimingState state = SystemRender::getGpuTimingState();
    App& app = call->ctx().app();
    // arm what is off and wait for a frame that has it: GPU times arrive a few frames late
    const uint64_t armedAt = FrameTimes::currentFrame();
    const bool needGpu = state.available;
    if (needGpu)
        SystemRender::leaseGpuTiming(4.0);
    if (includeDraws)
        SystemRender::setDrawListEnabled(true);
    app.requestContinuousFrames(1.0);
    call->waitUntil([tag, armedAt, needGpu, includeDraws, &app](){
        app.requestContinuousFrames(1.0);
        const PassList list = SystemRender::getPassList(tag, needGpu);
        if (list.frame <= armedAt + 1 || list.passes.empty())
            return false;
        if (needGpu && list.gpuPending)
            return false;
        if (needGpu){
            bool timed = false;
            for (const PassRecord& pass : list.passes)
                timed = timed || pass.gpuValid || (!pass.executed);
            if (!timed && !SystemRender::getGpuTimingState().passReason.empty())
                return true;
            if (!timed)
                return false;
        }
        return !includeDraws || !list.draws.empty() || list.passes.empty();
    }, 6000, [call, tag, name, includeDraws, maxDraws](bool){
        if (includeDraws)
            SystemRender::setDrawListEnabled(false);
        replyPasses(call, tag, name, includeDraws, maxDraws);
    });
}

// ------------------------------------------------------------------ get_vram_report

void getVramReportTool(const CallPtr& call){
    const Json& args = call->args();
    const std::string groupBy = args.value("group_by", std::string("category"));
    const int top = std::clamp(args.value("top", 30), 0, 1000);
    const std::string filter = args.value("filter", std::string());
    gpumem::Report report = gpumem::report(true);
    std::vector<const gpumem::Allocation*> kept;
    for (const gpumem::Allocation& allocation : report.allocations){
        if (filter.empty() || matches(allocation.owner, filter) || matches(allocation.label, filter) ||
            matches(gpumem::categoryName(allocation.category), filter) || matches(gpumem::categoryKey(allocation.category), filter))
            kept.push_back(&allocation);
    }
    Json groups = Json::array();
    if (groupBy == "owner"){
        gpumem::Report filtered;
        for (const gpumem::Allocation* allocation : kept)
            filtered.allocations.push_back(*allocation);
        for (const gpumem::Group& group : gpumem::groupByOwner(filtered)){
            if ((int)groups.size() >= std::max(top, 1))
                break;
            groups.push_back({{"name", group.name}, {"bytes", group.bytes}, {"count", group.count}});
        }
    }else if (groupBy == "category"){
        uint64_t bytes[gpumem::CATEGORY_COUNT] = {};
        uint32_t counts[gpumem::CATEGORY_COUNT] = {};
        for (const gpumem::Allocation* allocation : kept){
            bytes[(size_t)allocation->category] += allocation->bytes;
            counts[(size_t)allocation->category]++;
        }
        for (size_t c = 0; c < gpumem::CATEGORY_COUNT; c++){
            if (counts[c])
                groups.push_back({{"name", gpumem::categoryName((gpumem::Category)c)}, {"key", gpumem::categoryKey((gpumem::Category)c)},
                                  {"bytes", bytes[c]}, {"count", counts[c]}});
        }
        std::sort(groups.begin(), groups.end(), [](const Json& a, const Json& b){ return a["bytes"].get<uint64_t>() > b["bytes"].get<uint64_t>(); });
    }else{
        call->fail("group_by must be \"category\" or \"owner\".");
        return;
    }
    Json allocations = Json::array();
    for (const gpumem::Allocation* allocation : kept){
        if ((int)allocations.size() >= top)
            break;
        Json entry = {{"id", allocation->id}, {"kind", gpumem::kindName(allocation->kind)},
                      {"category", gpumem::categoryKey(allocation->category)}, {"owner", allocation->owner},
                      {"label", allocation->label}, {"bytes", allocation->bytes}, {"tagged", allocation->tagged}};
        if (allocation->image){
            entry["size"] = {allocation->width, allocation->height};
            entry["format"] = allocation->format;
            if (allocation->mips > 1) entry["mips"] = allocation->mips;
            if (allocation->slices > 1) entry["slices"] = allocation->slices;
            if (allocation->samples > 1) entry["samples"] = allocation->samples;
        }
        allocations.push_back(entry);
    }
    Json driver = nullptr;
    if (report.driver.known){
        driver = {{"source", report.driver.source}, {"used_bytes", report.driver.usedKnown ? Json(report.driver.usedBytes) : Json(nullptr)},
                  {"budget_bytes", report.driver.budgetBytes ? Json(report.driver.budgetBytes) : Json(nullptr)},
                  {"available_bytes", report.driver.availableBytes ? Json(report.driver.availableBytes) : Json(nullptr)}};
    }
    Json data = {{"estimated", true}, {"total_bytes", report.totalBytes}, {"peak_bytes", report.peakBytes},
                 {"count", report.count}, {"untagged_bytes", report.untaggedBytes}, {"group_by", groupBy},
                 {"groups", groups}, {"allocations", allocations}, {"driver", driver}};
    char text[160];
    std::snprintf(text, sizeof(text), "%.1f MB estimated in %u resources (peak %.1f MB)%s.", report.totalBytes / (1024.0 * 1024.0),
                  report.count, report.peakBytes / (1024.0 * 1024.0), report.driver.known ? "" : "; the driver reports n/a");
    call->ok(text, data);
}

} // namespace

// the shared entry of jasper://stats
Json profilerRenderStatsJson(){
    return renderStatsJson(FRAME_TAG, "frame");
}

void registerProfilerTools(McpRegistry& registry){
    {
        ToolDef tool;
        tool.name = "get_render_stats";
        tool.title = "Get Render Stats";
        tool.description =
            "A view's last drawn frame: draws, instances, triangles, passes, pipelines/bindings, uniform and upload bytes, alive "
            "resources, pipeline cache, cpu_ms, fps, gpu{available,reason,frame_ms,semantics}. One frame is one sample. "
            "gpu.frame_ms is null until GPU timing ran (get_gpu_passes arms it). Also jasper://stats.";
        tool.inputSchema = object({{"view", viewSchema()}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getRenderStatsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_frame_times";
        tool.title = "Get Frame Times";
        tool.description =
            "Frame-time history (always on, 600 frames; the editor only counts frames it draws): median/p95/p99/max of frame_ms, "
            "cpu_ms, gpu_ms, fps over `window`; min_frame_ms lists spikes and spike_interval_frames; `frames` newest samples; "
            "`monitors` named series (FPS, Draw calls, VRAM estimated...).";
        tool.inputSchema = object({
            {"window", withDefault(integer("", 1, 600), 300)},
            {"min_frame_ms", number("Spike threshold.")},
            {"frames", withDefault(integer("", 0, 600), 20)},
            {"monitors", array(string(""), "Monitor names.")}
        });
        tool.annotations.readOnly = true;
        tool.handler = getFrameTimesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "profiler_capture";
        tool.title = "Profiler Capture";
        tool.description =
            "Records `frames` frames (views redraw meanwhile), then replies: capture_id, frame/cpu/gpu ms summaries, per-category "
            "ms, worst_frame, overhead_note (recorded times include the profiler's cost), `path` when save_path is given "
            "(.jetrace.json: Perfetto/chrome://tracing). The frame after arming is skipped. gpu: time passes too.";
        tool.inputSchema = object({
            {"frames", withDefault(integer("", 1, 600), 120)},
            {"gpu", withDefault(boolean(""), true)},
            {"deep_lua", withDefault(boolean("A scope per Lua callback."), false)},
            {"save_path", string("Project-relative or absolute.")}
        });
        tool.annotations.readOnly = true;
        tool.activity = ToolActivity::Control;
        tool.handler = profilerCaptureTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_cpu_profile";
        tool.title = "Get CPU Profile";
        tool.description =
            "CPU scopes of one frame of a capture (or of the live recording): tree rows {name,depth,total_ms,self_ms,calls} of a "
            "thread (default main), flat rows with per-thread totals, or per-thread busy %. Job workers are 'Job Worker #n'.";
        tool.inputSchema = object({
            {"capture_id", integer("")},
            {"frame", Json{{"type", Json::array({"integer", "string"})}, {"description", "Index, latest, worst or median."}}},
            {"thread", Json{{"type", Json::array({"integer", "string"})}, {"description", "Name or id."}}},
            {"view", withDefault(stringEnum("", {"tree", "flat", "threads"}), "tree")},
            {"filter", string("Name substring.")},
            {"max_rows", withDefault(integer("", 1, 1000), 64)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getCpuProfileTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_gpu_passes";
        tool.title = "Get GPU Passes";
        tool.description =
            "The ordered sokol passes of a view's frame: targets, actions, draws, triangles, cpu_ms, gpu_ms, valid; arms GPU timing "
            "and waits for times (a few frames late). semantics: pipeline_point (Vulkan/D3D11), elapsed (GL: frame = sum), "
            "encoder_span (Metal: overlapping, do not sum). capture_id: per-label gpu_ms over the capture.";
        tool.inputSchema = object({
            {"view", viewSchema()},
            {"capture_id", integer("")},
            {"include_draws", withDefault(boolean(""), false)},
            {"max_draws", withDefault(integer("", 1, 5000), 200)}
        });
        tool.annotations.readOnly = true;
        tool.handler = getGpuPassesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_vram_report";
        tool.title = "Get VRAM Report";
        tool.description =
            "GPU memory of every image and buffer: estimated bytes (padding not modelled), by category or owner, the top "
            "allocations, peak, untagged_bytes and the driver's used/budget bytes (null where it reports none).";
        tool.inputSchema = object({
            {"group_by", withDefault(stringEnum("", {"category", "owner"}), "category")},
            {"top", withDefault(integer("", 0, 1000), 30)},
            {"filter", string("Owner, label or category substring.")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getVramReportTool;
        registry.addTool(std::move(tool));
    }
    {
        ResourceDef def;
        def.uri = "jasper://stats";
        def.name = "stats";
        def.title = "Render Stats";
        def.description = "get_render_stats of the engine's frame.";
        def.mimeType = "application/json";
        def.read = [](const std::string& uri){
            ResourceResult result;
            result.contents.push_back({uri, "application/json", profilerRenderStatsJson().dump(), ""});
            return result;
        };
        registry.addResource(std::move(def));
    }
    {
        ResourceTemplateDef def;
        def.uriTemplate = "jasper://profiler/capture/{id}";
        def.name = "profiler-capture";
        def.title = "Profiler Capture";
        def.description = "A profiler_capture as .jetrace.json (Chrome Trace Event JSON).";
        def.mimeType = "application/json";
        def.read = [](const std::string& uri, const std::map<std::string, std::string>& vars){
            ResourceResult result;
            auto it = vars.find("id");
            const profiler::CaptureId id = it != vars.end() ? (profiler::CaptureId)std::strtoul(it->second.c_str(), nullptr, 10) : 0;
            const std::string text = profiler::captureJson(id);
            if (text.empty()){
                result.ok = false;
                result.error = "No profiler capture " + (it != vars.end() ? it->second : std::string());
                return result;
            }
            result.contents.push_back({uri, "application/json", text, ""});
            return result;
        };
        registry.addResourceTemplate(std::move(def));
    }
}

}
