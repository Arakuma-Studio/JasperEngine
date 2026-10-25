// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProfilerWindow.h"

#include "App.h"
#include "Backend.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "profiler/FrameTimes.h"
#include "render/SystemRender.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "nfd.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const int BUDGETS[] = {0, 30, 60, 90, 120, 144, 240};

    ImU32 categoryColor(size_t category){
        static const ImU32 colors[profiler::CATEGORY_COUNT] = {
            IM_COL32(102, 187, 106, 255),   // Rendering
            IM_COL32(66, 165, 245, 255),    // Scripts
            IM_COL32(255, 167, 38, 255),    // Physics
            IM_COL32(171, 71, 188, 255),    // Animation
            IM_COL32(38, 198, 218, 255),    // UI
            IM_COL32(255, 238, 88, 255),    // Loading
            IM_COL32(38, 166, 154, 255),    // Jobs
            IM_COL32(120, 120, 120, 255),   // Wait
            IM_COL32(176, 176, 176, 255),   // Other
        };
        return colors[std::min(category, profiler::CATEGORY_COUNT - 1)];
    }

    // the GPU Usage chart's pass families, from the pass labels
    const char* const FAMILIES[] = {"Shadows", "Depth prepass", "GBuffer", "Opaque", "Transparent", "SSAO", "SSR",
                                    "Bloom", "PostFX", "Atmosphere", "UI", "Other"};
    constexpr size_t FAMILY_COUNT = sizeof(FAMILIES) / sizeof(FAMILIES[0]);

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    size_t familyOf(const std::string& label){
        const std::string name = lower(label);
        auto has = [&](const char* word){ return name.find(word) != std::string::npos; };
        if (has("shadow")) return 0;
        if (has("depth")) return 1;
        if (has("gbuffer")) return 2;
        if (has("transparent")) return 4;
        if (has("ssao")) return 5;
        if (has("ssr")) return 6;
        if (has("bloom")) return 7;
        if (has("post") || has("fxaa") || has("uber") || has("composite") || has("blit") || has("lut")) return 8;
        if (has("fog") || has("cloud") || has("atmos") || has("sky")) return 9;
        if (has("ui") || has("html") || has("rml")) return 10;
        if (has("camera") || has("scene") || has("opaque") || has("main")) return 3;
        return 11;
    }

    ImU32 familyColor(size_t family){
        static const ImU32 colors[FAMILY_COUNT] = {
            IM_COL32(120, 144, 156, 255), IM_COL32(141, 110, 99, 255), IM_COL32(92, 107, 192, 255), IM_COL32(102, 187, 106, 255),
            IM_COL32(77, 182, 172, 255), IM_COL32(255, 202, 40, 255), IM_COL32(240, 98, 146, 255), IM_COL32(255, 138, 101, 255),
            IM_COL32(186, 104, 200, 255), IM_COL32(79, 195, 247, 255), IM_COL32(38, 198, 218, 255), IM_COL32(176, 176, 176, 255),
        };
        return colors[std::min(family, FAMILY_COUNT - 1)];
    }

    ImU32 seriesColor(size_t index){
        static const ImU32 colors[] = {IM_COL32(66, 165, 245, 255), IM_COL32(255, 167, 38, 255), IM_COL32(102, 187, 106, 255),
                                       IM_COL32(239, 83, 80, 255), IM_COL32(171, 71, 188, 255), IM_COL32(38, 198, 218, 255)};
        return colors[index % (sizeof(colors) / sizeof(colors[0]))];
    }

    std::string formatBytes(double bytes){
        char text[32];
        if (bytes >= 1024.0 * 1024.0 * 1024.0)
            std::snprintf(text, sizeof(text), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        else if (bytes >= 1024.0 * 1024.0)
            std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
        else if (bytes >= 1024.0)
            std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
        else
            std::snprintf(text, sizeof(text), "%.0f B", bytes);
        return text;
    }

    bool containsText(const std::string& text, const char* pattern){
        if (!pattern || !pattern[0])
            return true;
        return lower(text).find(lower(pattern)) != std::string::npos;
    }

    float monitorValue(const profiler::Frame& frame, const std::vector<std::string>& names, const char* name){
        for (size_t i = 0; i < names.size() && i < frame.monitorValues.size(); i++){
            if (names[i] == name)
                return frame.monitorValues[i];
        }
        return std::nanf("");
    }

    // A chart of one value stack per frame: bars left to right, the newest at the right end, the
    // budget line, the selected frame's marker. Returns the frame index clicked, else -1.
    int drawChart(const char* id, const std::vector<const profiler::Frame*>& frames, size_t stackSize,
                  const std::function<void(const profiler::Frame&, std::vector<float>&)>& values,
                  const std::function<ImU32(size_t)>& colors, float maxValue, float budget, int selectedIndex,
                  const std::function<std::string(const profiler::Frame&, const std::vector<float>&)>& tooltip){
        const ImVec2 size(ImGui::GetContentRegionAvail().x, Theme::dpi(64.0f));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(28, 28, 28, 255));
        if (frames.empty() || maxValue <= 0.0f)
            return -1;
        const size_t visible = std::min<size_t>(frames.size(), std::max<size_t>(1, (size_t)(size.x / Theme::dpi(2.0f))));
        const size_t first = frames.size() - visible;
        const float barWidth = size.x / (float)visible;
        std::vector<float> stack(stackSize, 0.0f);
        for (size_t i = first; i < frames.size(); i++){
            values(*frames[i], stack);
            float bottom = origin.y + size.y;
            const float x0 = origin.x + (float)(i - first) * barWidth;
            for (size_t k = 0; k < stackSize; k++){
                if (!(stack[k] > 0.0f))
                    continue;
                const float h = std::min(stack[k] / maxValue, 1.0f) * size.y;
                draw->AddRectFilled(ImVec2(x0, bottom - h), ImVec2(x0 + std::max(barWidth - 0.5f, 1.0f), bottom), colors(k));
                bottom -= h;
            }
        }
        if (budget > 0.0f && budget < maxValue){
            const float y = origin.y + size.y - budget / maxValue * size.y;
            draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), IM_COL32(255, 255, 255, 140), 1.0f);
        }
        char label[48];
        std::snprintf(label, sizeof(label), "%.2f", maxValue);
        draw->AddText(ImVec2(origin.x + 3.0f, origin.y + 1.0f), IM_COL32(200, 200, 200, 160), label);
        if (selectedIndex >= (int)first){
            const float x = origin.x + ((float)(selectedIndex - (int)first) + 0.5f) * barWidth;
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), IM_COL32(255, 255, 255, 230), 1.5f);
        }
        int hovered = -1;
        if (ImGui::IsItemHovered()){
            hovered = (int)first + std::clamp((int)((ImGui::GetIO().MousePos.x - origin.x) / barWidth), 0, (int)visible - 1);
            values(*frames[(size_t)hovered], stack);
            ImGui::SetTooltip("%s", tooltip(*frames[(size_t)hovered], stack).c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                return hovered;
        }
        return -1;
    }

}

ProfilerWindow::ProfilerWindow(App* app): app(app){
}

void ProfilerWindow::setOpen(bool open){
    if (open && !windowOpen)
        focusRequested = true;
    windowOpen = open;
}

bool ProfilerWindow::isRecording() const{
    return profiler::isEnabled();
}

void ProfilerWindow::setRecording(bool recording){
    profiler::setEnabled(recording);
    if (recording && !gpuHeld){
        SystemRender::acquireGpuTiming();
        profiler::setGpuSemantics(SystemRender::getGpuTimingState().semantics);
        gpuHeld = true;
    }else if (!recording && gpuHeld){
        SystemRender::releaseGpuTiming();
        gpuHeld = false;
    }
    if (recording)
        target = 0;
}

bool ProfilerWindow::loadCapture(const std::string& path, std::string& error){
    const profiler::CaptureId id = profiler::loadCapture(path, error);
    if (!id)
        return false;
    target = id;
    selectedFrame = 0;
    statusMessage = "Loaded " + path;
    return true;
}

void ProfilerWindow::setTarget(uint32_t captureId){
    target = captureId;
    selectedFrame = 0;
}

void ProfilerWindow::selectFrame(uint64_t frame){
    selectedFrame = frame;
}

size_t ProfilerWindow::targetFrameCount() const{
    return frames().size();
}

std::vector<const profiler::Frame*> ProfilerWindow::frames() const{
    std::vector<const profiler::Frame*> out;
    if (target){
        if (const std::vector<profiler::Frame>* captured = profiler::captureFrames(target)){
            for (const profiler::Frame& frame : *captured)
                out.push_back(&frame);
        }
        return out;
    }
    for (size_t i = 0; i < profiler::frameCount(); i++)
        out.push_back(profiler::frameAt(i));
    return out;
}

const profiler::Frame* ProfilerWindow::currentFrame(const std::vector<const profiler::Frame*>& list) const{
    if (list.empty())
        return nullptr;
    if (selectedFrame){
        for (auto it = list.rbegin(); it != list.rend(); ++it){
            if ((*it)->index == selectedFrame)
                return *it;
        }
    }
    return list.back();
}

std::string ProfilerWindow::nameOf(uint32_t nameId) const{
    return target ? profiler::captureNameOf(target, nameId) : profiler::nameOf(nameId);
}

std::vector<profiler::ThreadInfo> ProfilerWindow::threadList() const{
    return target ? profiler::captureThreads(target) : profiler::threads();
}

std::vector<std::string> ProfilerWindow::monitorNameList() const{
    return target ? profiler::captureMonitorNames(target) : profiler::monitorNames();
}

void ProfilerWindow::storeSettings(){
    AppSettings::setProfilerSettings(settings);
}

void ProfilerWindow::show(){
    windowFocused = false;
    if (!windowOpen){
        if (gpuHeld && !profiler::isEnabled()){
            SystemRender::releaseGpuTiming();
            gpuHeld = false;
        }
        return;
    }
    if (!settingsLoaded){
        settings = AppSettings::getProfilerSettings();
        settingsLoaded = true;
    }
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(900.0f, 640.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)){
        ImGui::End();
        return;
    }
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // a capture of another source ended: the GPU timing this window holds follows Record
    if (gpuHeld && !profiler::isEnabled()){
        SystemRender::releaseGpuTiming();
        gpuHeld = false;
    }

    const std::vector<const profiler::Frame*> list = frames();

    // pause on a frame over the threshold: recording stops on it
    if (!target && profiler::isEnabled() && settings.pauseOnSpike && !list.empty() &&
        list.back()->frameMs() > settings.pauseOverMs && !list.back()->partial){
        setRecording(false);
        selectedFrame = list.back()->index;
        statusMessage = "Paused on frame " + std::to_string(selectedFrame) + " (" +
                        std::to_string((int)std::lround(list.back()->frameMs())) + " ms)";
    }

    drawToolbar(list);
    ImGui::Separator();
    const float available = ImGui::GetContentRegionAvail().y;
    drawModules(list, std::max(Theme::dpi(120.0f), available * 0.45f));
    ImGui::Separator();
    drawDetail(list);
    ImGui::End();
}

void ProfilerWindow::drawToolbar(const std::vector<const profiler::Frame*>& list){
    const bool recording = profiler::isEnabled();
    if (recording)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button(recording ? ICON_FA_CIRCLE " Recording" : ICON_FA_CIRCLE " Record"))
        setRecording(!recording);
    if (recording)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Record the frames' CPU scopes, GPU pass times and monitors (the editor draws while it records)");
    ImGui::SameLine();
    if (ImGui::Button("Clear")){
        profiler::clearHistory();
        target = 0;
        selectedFrame = 0;
    }
    ImGui::SameLine();
    bool deep = profiler::isDeepLua();
    if (ImGui::Checkbox("Deep Profile Lua", &deep))
        profiler::setDeepLua(deep);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A scope per Lua callback (more overhead)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    char budgetLabel[32];
    std::snprintf(budgetLabel, sizeof(budgetLabel), settings.budgetFps ? "%d fps" : "No budget", settings.budgetFps);
    if (ImGui::BeginCombo("##Budget", budgetLabel)){
        for (int fps : BUDGETS){
            char item[32];
            std::snprintf(item, sizeof(item), fps ? "%d fps (%.1f ms)" : "No budget", fps, fps ? 1000.0 / fps : 0.0);
            if (ImGui::Selectable(item, settings.budgetFps == fps)){
                settings.budgetFps = fps;
                storeSettings();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Frame budget: the line on the charts");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Load"))
        loadFromDialog();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save"))
        saveTarget();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(150.0f));
    std::string targetLabel = target ? "Capture " + std::to_string(target) : std::string(app && app->getProject() &&
        app->getProject()->isPlaySessionActive() ? "Play Mode" : "Editor");
    if (ImGui::BeginCombo("##Target", targetLabel.c_str())){
        if (ImGui::Selectable(app && app->getProject() && app->getProject()->isPlaySessionActive() ? "Play Mode" : "Editor", target == 0))
            setTarget(0);
        for (const profiler::CaptureInfo& capture : profiler::captures()){
            if (!capture.complete)
                continue;
            std::string label = "Capture " + std::to_string(capture.id) + " (" + std::to_string(capture.framesRecorded) + " frames" +
                (capture.loaded ? ", file" : "") + ")";
            if (ImGui::Selectable(label.c_str(), target == capture.id))
                setTarget(capture.id);
        }
        ImGui::EndCombo();
    }

    // the frame scrub
    ImGui::SameLine();
    int index = -1;
    const profiler::Frame* current = currentFrame(list);
    for (size_t i = 0; i < list.size(); i++){
        if (list[i] == current)
            index = (int)i;
    }
    if (ImGui::ArrowButton("##Prev", ImGuiDir_Left) && index > 0)
        selectedFrame = list[(size_t)index - 1]->index;
    ImGui::SameLine();
    if (ImGui::ArrowButton("##Next", ImGuiDir_Right) && index >= 0 && index + 1 < (int)list.size())
        selectedFrame = list[(size_t)index + 1]->index;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    int slider = std::max(index, 0);
    char frameLabel[64];
    std::snprintf(frameLabel, sizeof(frameLabel), current ? "Frame %llu" : "No frames", current ? (unsigned long long)current->index : 0ull);
    if (ImGui::SliderInt("##Frame", &slider, 0, std::max((int)list.size() - 1, 0), frameLabel) && !list.empty())
        selectedFrame = list[(size_t)slider]->index;
    ImGui::SameLine();
    if (ImGui::Button("Current"))
        selectedFrame = 0;
    ImGui::SameLine();
    if (ImGui::Checkbox("Pause on frame >", &settings.pauseOnSpike))
        storeSettings();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(60.0f));
    if (ImGui::DragFloat("ms##Pause", &settings.pauseOverMs, 0.5f, 1.0f, 10000.0f, "%.0f"))
        storeSettings();

    // the measurement's caveat
    if (recording && current && current->frameMs() > 0.0){
        const double overheadMs = (double)current->events / 2.0 * profiler::scopePairCostNs() / 1.0e6;
        ImGui::TextDisabled("Recording: +%.1f%% overhead (estimated, %u events)%s", overheadMs * 100.0 / current->frameMs(),
                            current->events, current->dropped ? ", events dropped: a ring was full" : "");
    }else if (!statusMessage.empty()){
        ImGui::TextDisabled("%s", statusMessage.c_str());
    }else if (list.empty()){
        ImGui::TextDisabled("Record to see where the frame's time goes.");
    }else{
        ImGui::TextDisabled("%zu frames", list.size());
    }
}

void ProfilerWindow::drawModules(const std::vector<const profiler::Frame*>& list, float height){
    ImGui::BeginChild("##ProfilerModules", ImVec2(0.0f, height), false);
    const float budgetMs = settings.budgetFps ? 1000.0f / (float)settings.budgetFps : 0.0f;
    const profiler::Frame* current = currentFrame(list);
    int selectedIndex = -1;
    for (size_t i = 0; i < list.size(); i++){
        if (list[i] == current)
            selectedIndex = (int)i;
    }
    const std::vector<std::string> monitorList = monitorNameList();
    int clicked = -1;

    auto moduleRow = [&](Module module, const char* name, bool* shown, const std::function<int()>& chart){
        if (!*shown)
            return;
        ImGui::PushID(name);
        if (ImGui::BeginTable("##Row", 2, ImGuiTableFlags_SizingFixedFit)){
            ImGui::TableSetupColumn("##Name", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(150.0f));
            ImGui::TableSetupColumn("##Chart", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(name, selected == module, 0, ImVec2(0.0f, Theme::dpi(64.0f))))
                selected = module;
            if (ImGui::BeginPopupContextItem()){
                if (ImGui::MenuItem("Hide module")){
                    *shown = false;
                    storeSettings();
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            const int result = chart();
            if (result >= 0){
                clicked = result;
                selected = module;
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    };

    float cpuMax = budgetMs * 1.25f;
    for (const profiler::Frame* frame : list)
        cpuMax = std::max(cpuMax, (float)frame->frameMs());
    cpuMax = std::max(cpuMax, 1.0f);
    moduleRow(Module::Cpu, "CPU Usage", &settings.showCpu, [&](){
        return drawChart("##CpuChart", list, profiler::CATEGORY_COUNT, [](const profiler::Frame& frame, std::vector<float>& out){
            for (size_t c = 0; c < profiler::CATEGORY_COUNT; c++)
                out[c] = (float)frame.categoryMs[c];
        }, categoryColor, cpuMax, budgetMs, selectedIndex, [](const profiler::Frame& frame, const std::vector<float>& values){
            std::string text = "Frame " + std::to_string(frame.index) + ": " + std::to_string(frame.frameMs()).substr(0, 6) + " ms";
            for (size_t c = 0; c < profiler::CATEGORY_COUNT; c++){
                if (values[c] > 0.005f)
                    text += "\n" + std::string(profiler::categoryName((profiler::Category)c)) + "  " + std::to_string(values[c]).substr(0, 5) + " ms";
            }
            return text;
        });
    });

    const GpuTimingState gpu = SystemRender::getGpuTimingState();
    float gpuMax = budgetMs * 1.25f;
    for (const profiler::Frame* frame : list)
        gpuMax = std::max(gpuMax, (float)frame->gpuMs);
    gpuMax = std::max(gpuMax, 0.5f);
    moduleRow(Module::Gpu, "GPU Usage", &settings.showGpu, [&](){
        if (!gpu.available && !target){
            ImGui::TextDisabled("GPU timing unavailable: %s", gpu.reason.c_str());
            return -1;
        }
        return drawChart("##GpuChart", list, FAMILY_COUNT, [](const profiler::Frame& frame, std::vector<float>& out){
            std::fill(out.begin(), out.end(), 0.0f);
            for (const profiler::GpuPassSample& pass : frame.gpu){
                if (pass.valid && pass.viewTag == SystemRender::FRAME_STATS_FRAME)
                    out[familyOf(pass.label)] += (float)pass.gpuMs;
            }
        }, familyColor, gpuMax, budgetMs, selectedIndex, [](const profiler::Frame& frame, const std::vector<float>& values){
            std::string text = "Frame " + std::to_string(frame.index) + ": GPU " +
                (frame.gpuMs >= 0.0 ? std::to_string(frame.gpuMs).substr(0, 6) + " ms" : std::string("n/a"));
            for (size_t f = 0; f < FAMILY_COUNT; f++){
                if (values[f] > 0.0005f)
                    text += "\n" + std::string(FAMILIES[f]) + "  " + std::to_string(values[f]).substr(0, 6) + " ms";
            }
            return text;
        });
    });

    const char* const renderingSeries[] = {"Draw calls", "Passes", "Pipelines applied"};
    float renderingMax = 1.0f;
    for (const profiler::Frame* frame : list){
        for (const char* name : renderingSeries){
            const float value = monitorValue(*frame, monitorList, name);
            if (value > renderingMax)
                renderingMax = value;
        }
    }
    moduleRow(Module::Rendering, "Rendering", &settings.showRendering, [&](){
        return drawChart("##RenderingChart", list, 1, [&](const profiler::Frame& frame, std::vector<float>& out){
            out[0] = monitorValue(frame, monitorList, "Draw calls");
        }, [](size_t){ return IM_COL32(66, 165, 245, 255); }, renderingMax, 0.0f, selectedIndex,
        [&](const profiler::Frame& frame, const std::vector<float>&){
            std::string text = "Frame " + std::to_string(frame.index);
            for (const char* name : {"Draw calls", "Triangles", "Passes", "Pipelines applied", "HTML UI draws"}){
                const float value = monitorValue(frame, monitorList, name);
                if (!std::isnan(value))
                    text += "\n" + std::string(name) + "  " + std::to_string((long long)value);
            }
            return text;
        });
    });

    std::vector<std::string> vramNames;
    for (size_t c = 0; c < gpumem::CATEGORY_COUNT; c++)
        vramNames.push_back(std::string("VRAM ") + gpumem::categoryName((gpumem::Category)c));
    float memoryMax = 1.0f;
    for (const profiler::Frame* frame : list){
        const float total = monitorValue(*frame, monitorList, "VRAM estimated");
        if (total > memoryMax)
            memoryMax = total;
    }
    moduleRow(Module::Memory, "GPU Memory", &settings.showMemory, [&](){
        return drawChart("##MemoryChart", list, gpumem::CATEGORY_COUNT, [&](const profiler::Frame& frame, std::vector<float>& out){
            for (size_t c = 0; c < gpumem::CATEGORY_COUNT; c++){
                const float value = monitorValue(frame, monitorList, vramNames[c].c_str());
                out[c] = std::isnan(value) ? 0.0f : value;
            }
        }, [](size_t c){ return categoryColor(c % profiler::CATEGORY_COUNT); }, memoryMax, 0.0f, selectedIndex,
        [&](const profiler::Frame& frame, const std::vector<float>& values){
            std::string text = "Frame " + std::to_string(frame.index) + ": " + formatBytes(monitorValue(frame, monitorList, "VRAM estimated")) + " estimated";
            for (size_t c = 0; c < gpumem::CATEGORY_COUNT; c++){
                if (values[c] > 0.0f)
                    text += "\n" + std::string(gpumem::categoryName((gpumem::Category)c)) + "  " + formatBytes(values[c]);
            }
            return text;
        });
    });

    moduleRow(Module::Monitors, "Monitors", &settings.showMonitors, [&](){
        // each chosen series scaled to its own maximum
        std::vector<float> maxima(settings.monitors.size(), 0.0f);
        for (const profiler::Frame* frame : list){
            for (size_t m = 0; m < settings.monitors.size(); m++){
                const float value = monitorValue(*frame, monitorList, settings.monitors[m].c_str());
                if (value > maxima[m])
                    maxima[m] = value;
            }
        }
        const ImVec2 size(ImGui::GetContentRegionAvail().x, Theme::dpi(64.0f));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##MonitorsChart", size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(28, 28, 28, 255));
        if (list.empty())
            return -1;
        const size_t visible = std::min<size_t>(list.size(), std::max<size_t>(2, (size_t)(size.x / Theme::dpi(2.0f))));
        const size_t first = list.size() - visible;
        const float step = size.x / (float)(visible - 1 > 0 ? visible - 1 : 1);
        for (size_t m = 0; m < settings.monitors.size(); m++){
            if (maxima[m] <= 0.0f)
                continue;
            ImVec2 previous;
            bool havePrevious = false;
            for (size_t i = first; i < list.size(); i++){
                const float value = monitorValue(*list[i], monitorList, settings.monitors[m].c_str());
                if (std::isnan(value)){
                    havePrevious = false;
                    continue;
                }
                const ImVec2 point(origin.x + (float)(i - first) * step, origin.y + size.y - value / maxima[m] * (size.y - 2.0f));
                if (havePrevious)
                    draw->AddLine(previous, point, seriesColor(m), 1.5f);
                previous = point;
                havePrevious = true;
            }
        }
        float legendX = origin.x + 4.0f;
        for (size_t m = 0; m < settings.monitors.size(); m++){
            draw->AddText(ImVec2(legendX, origin.y + 2.0f), seriesColor(m), settings.monitors[m].c_str());
            legendX += ImGui::CalcTextSize(settings.monitors[m].c_str()).x + Theme::dpi(12.0f);
        }
        if (ImGui::IsItemHovered()){
            const int hovered = (int)first + std::clamp((int)std::lround((ImGui::GetIO().MousePos.x - origin.x) / step), 0, (int)visible - 1);
            std::string text = "Frame " + std::to_string(list[(size_t)hovered]->index);
            for (const std::string& name : settings.monitors){
                const float value = monitorValue(*list[(size_t)hovered], monitorList, name.c_str());
                if (!std::isnan(value))
                    text += "\n" + name + "  " + std::to_string(value).substr(0, 10);
            }
            ImGui::SetTooltip("%s", text.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                return hovered;
        }
        return -1;
    });

    // the hidden modules come back from here
    if (!settings.showCpu || !settings.showGpu || !settings.showRendering || !settings.showMemory || !settings.showMonitors){
        if (ImGui::SmallButton("Add module"))
            ImGui::OpenPopup("##AddModule");
        if (ImGui::BeginPopup("##AddModule")){
            auto item = [&](const char* name, bool& shown){
                if (!shown && ImGui::MenuItem(name)){
                    shown = true;
                    storeSettings();
                }
            };
            item("CPU Usage", settings.showCpu);
            item("GPU Usage", settings.showGpu);
            item("Rendering", settings.showRendering);
            item("GPU Memory", settings.showMemory);
            item("Monitors", settings.showMonitors);
            ImGui::EndPopup();
        }
    }
    if (clicked >= 0)
        selectedFrame = list[(size_t)clicked]->index;
    ImGui::EndChild();
}

void ProfilerWindow::drawDetail(const std::vector<const profiler::Frame*>& list){
    ImGui::BeginChild("##ProfilerDetail", ImVec2(0.0f, 0.0f), false);
    const profiler::Frame* frame = currentFrame(list);
    if (selected == Module::Memory){
        drawMemoryDetail();
    }else if (selected == Module::Monitors){
        drawMonitorsDetail(list);
    }else if (!frame){
        ImGui::TextDisabled("No frame recorded.");
    }else if (selected == Module::Cpu){
        ImGui::Text("Frame %llu: %.2f ms (CPU %.2f ms%s)", (unsigned long long)frame->index, frame->frameMs(), frame->cpuMs,
                    frame->partial ? ", partial: the first after Record" : "");
        ImGui::SameLine();
        int view = settings.cpuView;
        if (ImGui::RadioButton("Hierarchy", view == 0)) view = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Timeline", view == 1)) view = 1;
        if (view != settings.cpuView){
            settings.cpuView = view;
            storeSettings();
        }
        if (!frame->hasDetail){
            ImGui::TextDisabled("This frame's scopes are no longer kept (the newest 300 frames are).");
        }else if (settings.cpuView == 0){
            drawCpuHierarchy(*frame);
        }else{
            drawCpuTimeline(*frame);
        }
    }else if (selected == Module::Gpu){
        drawGpuDetail(*frame);
    }else{
        drawRenderingDetail(*frame);
    }
    ImGui::EndChild();
}

void ProfilerWindow::drawCpuHierarchy(const profiler::Frame& frame){
    const std::vector<profiler::ThreadInfo> threads = threadList();
    if (!cpuThread){
        for (const profiler::ThreadInfo& info : threads){
            if (info.main)
                cpuThread = info.id;
        }
    }
    std::string threadName = "Thread " + std::to_string(cpuThread);
    for (const profiler::ThreadInfo& info : threads){
        if (info.id == cpuThread)
            threadName = info.name;
    }
    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    if (ImGui::BeginCombo("##Thread", threadName.c_str())){
        for (const profiler::ThreadTimeline& timeline : frame.threads){
            std::string name = "Thread " + std::to_string(timeline.thread);
            for (const profiler::ThreadInfo& info : threads){
                if (info.id == timeline.thread)
                    name = info.name;
            }
            if (ImGui::Selectable(name.c_str(), timeline.thread == cpuThread))
                cpuThread = timeline.thread;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS " Search", search, sizeof(search));

    const std::vector<profiler::TreeRow> rows = profiler::buildTree(frame, cpuThread, [this](uint32_t id){ return nameOf(id); });
    // the tree: each row's children, sorted by the column
    std::map<std::string, std::vector<size_t>> children;
    for (size_t i = 0; i < rows.size(); i++){
        const size_t slash = rows[i].path.rfind('/');
        children[slash == std::string::npos ? std::string() : rows[i].path.substr(0, slash)].push_back(i);
    }
    auto ascending = [&](size_t a, size_t b){
        const profiler::TreeRow& x = rows[a];
        const profiler::TreeRow& y = rows[b];
        switch (settings.sortColumn){
            case 0: return x.name < y.name;
            case 2: return x.selfMs < y.selfMs;
            case 3: return x.calls < y.calls;
            default: return x.totalMs < y.totalMs;
        }
    };
    auto less = [&](size_t a, size_t b){
        return settings.sortAscending ? ascending(a, b) : ascending(b, a);
    };
    for (auto& [parent, list] : children)
        std::stable_sort(list.begin(), list.end(), less);
    // with a search: the matching rows and their ancestors
    std::set<std::string> visible;
    if (search[0]){
        for (const profiler::TreeRow& row : rows){
            if (!containsText(row.name, search))
                continue;
            std::string path = row.path;
            while (!path.empty()){
                visible.insert(path);
                const size_t slash = path.rfind('/');
                path = slash == std::string::npos ? std::string() : path.substr(0, slash);
            }
        }
    }
    const double frameMs = std::max(frame.frameMs(), 1e-6);
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##Hierarchy", 5, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | (settings.sortColumn == 0 ? ImGuiTableColumnFlags_DefaultSort : 0));
    ImGui::TableSetupColumn("Total ms", ImGuiTableColumnFlags_WidthFixed | (settings.sortColumn == 1 ? ImGuiTableColumnFlags_DefaultSort : 0), Theme::dpi(80.0f));
    ImGui::TableSetupColumn("Self ms", ImGuiTableColumnFlags_WidthFixed | (settings.sortColumn == 2 ? ImGuiTableColumnFlags_DefaultSort : 0), Theme::dpi(80.0f));
    ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed | (settings.sortColumn == 3 ? ImGuiTableColumnFlags_DefaultSort : 0), Theme::dpi(60.0f));
    ImGui::TableSetupColumn("Total %", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, Theme::dpi(60.0f));
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()){
        if (sort->SpecsDirty && sort->SpecsCount > 0){
            settings.sortColumn = std::clamp((int)sort->Specs[0].ColumnIndex, 0, 3);
            settings.sortAscending = sort->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            sort->SpecsDirty = false;
            storeSettings();
        }
    }
    std::function<void(const std::string&)> drawChildren = [&](const std::string& parent){
        auto it = children.find(parent);
        if (it == children.end())
            return;
        for (size_t index : it->second){
            const profiler::TreeRow& row = rows[index];
            if (search[0] && !visible.count(row.path))
                continue;
            const bool hasChildren = children.count(row.path) > 0;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow;
            if (!hasChildren)
                nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (row.path == selectedPath)
                nodeFlags |= ImGuiTreeNodeFlags_Selected;
            if (search[0] || !collapsed.count(row.path))
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            const bool open = ImGui::TreeNodeEx(row.path.c_str(), nodeFlags, "%s", row.name.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                selectedPath = row.path;
            if (hasChildren){
                if (open) collapsed.erase(row.path);
                else collapsed.insert(row.path);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", row.totalMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", row.selfMs);
            ImGui::TableNextColumn();
            ImGui::Text("%u", row.calls);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f%%", row.totalMs * 100.0 / frameMs);
            if (hasChildren && open){
                drawChildren(row.path);
                ImGui::TreePop();
            }
        }
    };
    drawChildren(std::string());
    ImGui::EndTable();
}

void ProfilerWindow::drawCpuTimeline(const profiler::Frame& frame){
    const std::vector<profiler::ThreadInfo> threads = threadList();
    ImGui::TextDisabled("Wheel: zoom   Drag: pan   Click: select the scope in Hierarchy");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset view")){
        timelineZoom = 1.0f;
        timelinePan = 0.0f;
    }
    const float laneHeader = ImGui::GetTextLineHeightWithSpacing();
    const float rowHeight = ImGui::GetTextLineHeight() + Theme::dpi(2.0f);
    float totalHeight = 0.0f;
    for (const profiler::ThreadTimeline& timeline : frame.threads){
        uint16_t depth = 0;
        for (const profiler::Interval& interval : timeline.intervals)
            depth = std::max(depth, interval.depth);
        totalHeight += laneHeader + rowHeight * (float)(depth + 1) + Theme::dpi(4.0f);
    }
    ImGui::BeginChild("##Timeline", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##TimelineArea", ImVec2(width, std::max(totalHeight, Theme::dpi(40.0f))));
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    const double frameNs = std::max((double)(frame.endNs - frame.startNs), 1.0);
    auto xOf = [&](uint64_t ns){
        return origin.x + ((float)((double)(ns - frame.startNs) / frameNs) * width - timelinePan) * timelineZoom;
    };
    if (hovered && io.MouseWheel != 0.0f){
        // around the pointer
        const float before = (io.MousePos.x - origin.x) / timelineZoom + timelinePan;
        timelineZoom = std::clamp(timelineZoom * (io.MouseWheel > 0.0f ? 1.25f : 0.8f), 1.0f, 1000.0f);
        timelinePan = before - (io.MousePos.x - origin.x) / timelineZoom;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        timelinePan -= io.MouseDelta.x / timelineZoom;
    timelinePan = std::clamp(timelinePan, 0.0f, std::max(0.0f, width - width / timelineZoom));

    ImDrawList* draw = ImGui::GetWindowDrawList();
    float y = origin.y;
    for (const profiler::ThreadTimeline& timeline : frame.threads){
        std::string name = "Thread " + std::to_string(timeline.thread);
        for (const profiler::ThreadInfo& info : threads){
            if (info.id == timeline.thread)
                name = info.name;
        }
        draw->AddText(ImVec2(origin.x + 2.0f, y), IM_COL32(220, 220, 220, 255), name.c_str());
        y += laneHeader;
        uint16_t maxDepth = 0;
        // the path of each interval, for the Hierarchy selection
        std::vector<std::string> stack;
        for (const profiler::Interval& interval : timeline.intervals){
            maxDepth = std::max(maxDepth, interval.depth);
            stack.resize(interval.depth);
            const std::string label = nameOf(interval.nameId);
            stack.push_back(label);
            const float x0 = xOf(interval.startNs);
            const float x1 = std::max(xOf(interval.endNs), x0 + 1.0f);
            if (x1 < origin.x || x0 > origin.x + width)
                continue;
            const float top = y + rowHeight * (float)interval.depth;
            // a colour per name, so a scope reads the same on every lane
            const ImU32 color = categoryColor(std::hash<std::string>()(label) % 7);
            draw->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + rowHeight - 1.0f), color, 1.0f);
            std::string path;
            for (size_t i = 0; i < stack.size(); i++)
                path += (i ? "/" : "") + stack[i];
            if (path == selectedPath && timeline.thread == cpuThread)
                draw->AddRect(ImVec2(x0, top), ImVec2(x1, top + rowHeight - 1.0f), IM_COL32(255, 255, 255, 255), 1.0f, 0, 2.0f);
            if (x1 - x0 > ImGui::CalcTextSize(label.c_str()).x + 4.0f)
                draw->AddText(ImVec2(x0 + 2.0f, top), IM_COL32(20, 20, 20, 255), label.c_str());
            if (hovered && io.MousePos.x >= x0 && io.MousePos.x <= x1 && io.MousePos.y >= top && io.MousePos.y < top + rowHeight){
                ImGui::SetTooltip("%s\n%.3f ms\ndepth %u%s", label.c_str(), (double)(interval.endNs - interval.startNs) / 1.0e6,
                                  interval.depth, (interval.flags & profiler::INTERVAL_CONTINUED) ? "\n(continued from the last frame)" : "");
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left)){
                    selectedPath = path;
                    cpuThread = timeline.thread;
                    for (size_t i = 1; i < stack.size(); i++){
                        std::string ancestor;
                        for (size_t k = 0; k < i; k++)
                            ancestor += (k ? "/" : "") + stack[k];
                        collapsed.erase(ancestor);
                    }
                }
            }
        }
        y += rowHeight * (float)(maxDepth + 1) + Theme::dpi(4.0f);
        draw->AddLine(ImVec2(origin.x, y - Theme::dpi(2.0f)), ImVec2(origin.x + width, y - Theme::dpi(2.0f)), IM_COL32(60, 60, 60, 255));
    }
    ImGui::EndChild();
}

void ProfilerWindow::drawGpuDetail(const profiler::Frame& frame){
    const GpuTimingState gpu = SystemRender::getGpuTimingState();
    ImGui::Text("Frame %llu: GPU %s", (unsigned long long)frame.index,
                frame.gpuMs >= 0.0 ? (std::to_string(frame.gpuMs).substr(0, 6) + " ms").c_str() : "n/a");
    if (!gpu.available && !target)
        ImGui::TextColored(Theme::Colors::WarningText, "GPU timing unavailable: %s", gpu.reason.c_str());
    else if (gpu.semantics == "encoder_span")
        ImGui::TextDisabled("Metal: encoder spans overlap; do not sum.");
    else if (gpu.semantics == "elapsed")
        ImGui::TextDisabled("GL: one elapsed query per pass; the frame time is their sum.");
    else
        ImGui::TextDisabled("Timestamps at each pass's start and end (%s).", gpu.semantics.c_str());
    ImGui::TextDisabled("The editor's own ImGui rendering is not timed on the GPU (CPU only: Editor Render).");
    if (!ImGui::BeginTable("##GpuPasses", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(30.0f));
    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(70.0f));
    ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(70.0f));
    ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(60.0f));
    ImGui::TableHeadersRow();
    int index = 0;
    for (const profiler::GpuPassSample& pass : frame.gpu){
        if (pass.viewTag != SystemRender::FRAME_STATS_FRAME)
            continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", index++);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(pass.label.c_str());
        ImGui::TableNextColumn();
        if (pass.valid) ImGui::Text("%.4f", pass.gpuMs);
        else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", pass.cpuMs);
        ImGui::TableNextColumn();
        ImGui::Text("%u", pass.draws);
    }
    ImGui::EndTable();
    if (index == 0)
        ImGui::TextDisabled("No GPU pass times in this frame (they arrive a few frames late, while Record is on).");
}

void ProfilerWindow::drawRenderingDetail(const profiler::Frame& frame){
    const std::vector<std::string> names = monitorNameList();
    ImGui::Text("Frame %llu", (unsigned long long)frame.index);
    if (!ImGui::BeginTable("##Rendering", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        return;
    ImGui::TableSetupColumn("Counter", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(120.0f));
    ImGui::TableHeadersRow();
    for (const char* name : {"Draw calls", "Instances", "Triangles", "Passes", "Pipelines applied", "Uniform bytes",
                             "Image upload bytes", "Buffer upload bytes", "HTML UI draws"}){
        const float value = monitorValue(frame, names, name);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name);
        ImGui::TableNextColumn();
        if (std::isnan(value)) ImGui::TextDisabled("-");
        else if (std::strstr(name, "bytes")) ImGui::TextUnformatted(formatBytes(value).c_str());
        else ImGui::Text("%lld", (long long)value);
    }
    ImGui::EndTable();
}

void ProfilerWindow::drawMemoryDetail(){
    const uint64_t generation = gpumem::generation();
    if (generation != memoryGeneration){
        memoryReport = gpumem::report(true);
        memoryGeneration = generation;
    }
    const gpumem::Report& report = memoryReport;
    ImGui::Text("Estimated: %s in %u resources (peak %s)", formatBytes((double)report.totalBytes).c_str(), report.count,
                formatBytes((double)report.peakBytes).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset peak"))
        gpumem::resetPeak();
    if (report.driver.known){
        ImGui::TextDisabled("Driver (%s): %s used%s%s", report.driver.source,
                            report.driver.usedKnown ? formatBytes((double)report.driver.usedBytes).c_str() : "?",
                            report.driver.budgetBytes ? (", budget " + formatBytes((double)report.driver.budgetBytes)).c_str() : "",
                            report.driver.availableBytes ? (", available " + formatBytes((double)report.driver.availableBytes)).c_str() : "");
    }else{
        ImGui::TextDisabled("Driver: n/a");
    }
    // the categories as bars
    for (size_t c = 0; c < gpumem::CATEGORY_COUNT; c++){
        if (!report.categoryCount[c])
            continue;
        const float share = report.totalBytes ? (float)((double)report.categoryBytes[c] / (double)report.totalBytes) : 0.0f;
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%s  %s (%u)", gpumem::categoryName((gpumem::Category)c),
                      formatBytes((double)report.categoryBytes[c]).c_str(), report.categoryCount[c]);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(categoryColor(c % profiler::CATEGORY_COUNT)));
        ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::PopStyleColor();
    }
    int group = settings.memoryGroup;
    if (ImGui::RadioButton("By category", group == 0)) group = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("By owner", group == 1)) group = 1;
    if (group != settings.memoryGroup){
        settings.memoryGroup = group;
        storeSettings();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    ImGui::InputTextWithHint("##MemoryFilter", ICON_FA_MAGNIFYING_GLASS " Name, owner or category", memoryFilter, sizeof(memoryFilter));

    std::vector<const gpumem::Allocation*> rows;
    for (const gpumem::Allocation& allocation : report.allocations){
        if (containsText(allocation.label, memoryFilter) || containsText(allocation.owner, memoryFilter) ||
            containsText(gpumem::categoryName(allocation.category), memoryFilter))
            rows.push_back(&allocation);
    }
    auto ascending = [&](const gpumem::Allocation* a, const gpumem::Allocation* b){
        switch (memorySortColumn){
            case 0: return a->label < b->label;
            case 1: return a->owner < b->owner;
            case 2: return a->category < b->category;
            case 3: return a->kind < b->kind;
            case 4: return (int64_t)a->width * a->height < (int64_t)b->width * b->height;
            case 5: return a->format < b->format;
            default: return a->bytes < b->bytes;
        }
    };
    auto less = [&](const gpumem::Allocation* a, const gpumem::Allocation* b){
        return memorySortAscending ? ascending(a, b) : ascending(b, a);
    };
    std::stable_sort(rows.begin(), rows.end(), less);
    // grouped: the group's rows under its header row
    std::map<std::string, std::vector<const gpumem::Allocation*>> groups;
    std::vector<std::string> groupOrder;
    for (const gpumem::Allocation* allocation : rows){
        const std::string key = settings.memoryGroup == 0 ? gpumem::categoryName(allocation->category) : allocation->owner;
        if (!groups.count(key))
            groupOrder.push_back(key);
        groups[key].push_back(allocation);
    }
    auto groupBytes = [&](const std::string& key){
        uint64_t bytes = 0;
        for (const gpumem::Allocation* allocation : groups[key])
            bytes += allocation->bytes;
        return bytes;
    };
    std::stable_sort(groupOrder.begin(), groupOrder.end(), [&](const std::string& a, const std::string& b){ return groupBytes(a) > groupBytes(b); });

    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##Allocations", 7, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(100.0f));
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(90.0f));
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(110.0f));
    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(90.0f));
    ImGui::TableSetupColumn("Estimated", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, Theme::dpi(80.0f));
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()){
        if (sort->SpecsDirty && sort->SpecsCount > 0){
            memorySortColumn = sort->Specs[0].ColumnIndex;
            memorySortAscending = sort->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            sort->SpecsDirty = false;
        }
    }
    for (const std::string& key : groupOrder){
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool open = ImGui::TreeNodeEx(key.c_str(), ImGuiTreeNodeFlags_SpanAllColumns, "%s (%zu)", key.c_str(), groups[key].size());
        for (int column = 1; column < 6; column++)
            ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(formatBytes((double)groupBytes(key)).c_str());
        if (!open)
            continue;
        for (const gpumem::Allocation* allocation : groups[key]){
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(allocation->label.empty() ? "(unnamed)" : allocation->label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(allocation->owner.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(gpumem::categoryName(allocation->category));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(gpumem::kindName(allocation->kind));
            ImGui::TableNextColumn();
            if (allocation->image)
                ImGui::Text("%dx%d%s", allocation->width, allocation->height, allocation->slices > 1 ? (" x" + std::to_string(allocation->slices)).c_str() : "");
            else
                ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(allocation->format.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatBytes((double)allocation->bytes).c_str());
        }
        ImGui::TreePop();
    }
    ImGui::EndTable();
}

void ProfilerWindow::drawMonitorsDetail(const std::vector<const profiler::Frame*>& list){
    ImGui::TextDisabled("Check a monitor to chart it in the Monitors module.");
    if (!ImGui::BeginTable("##Monitors", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Chart", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(40.0f));
    ImGui::TableSetupColumn("Monitor", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(60.0f));
    ImGui::TableSetupColumn("Latest", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(90.0f));
    ImGui::TableSetupColumn("Average", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(90.0f));
    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(90.0f));
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(90.0f));
    ImGui::TableHeadersRow();
    for (const profiler::MonitorInfo& info : profiler::monitors()){
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        auto it = std::find(settings.monitors.begin(), settings.monitors.end(), info.name);
        bool charted = it != settings.monitors.end();
        if (ImGui::Checkbox(("##chart" + info.name).c_str(), &charted)){
            if (charted) settings.monitors.push_back(info.name);
            else settings.monitors.erase(it);
            storeSettings();
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(info.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(profiler::monitorUnitName(info.unit));
        auto value = [&](double v){
            ImGui::TableNextColumn();
            if (info.unit == profiler::MonitorUnit::Memory) ImGui::TextUnformatted(formatBytes(v).c_str());
            else ImGui::Text("%.2f", v);
        };
        value(info.latest);
        value(info.average);
        value(info.minimum);
        value(info.maximum);
    }
    ImGui::EndTable();
}

void ProfilerWindow::saveTarget(){
    profiler::CaptureId id = target;
    if (!id){
        id = profiler::captureFromHistory();
        if (!id){
            statusMessage = "Nothing to save: record frames first.";
            return;
        }
    }
    nfdu8char_t* outPath = nullptr;
    nfdsavedialogu8args_t args = {};
    nfdfilteritem_t filter = {"Profiler capture", "json"};
    args.filterList = &filter;
    args.filterCount = 1;
    args.defaultName = "capture.jetrace.json";
    args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());
    if (NFD_SaveDialogU8_With(&outPath, &args) != NFD_OKAY)
        return;
    std::string path = outPath;
    NFD_FreePathU8(outPath);
    std::string error;
    statusMessage = profiler::saveCapture(id, path, &error) ? "Saved " + path : "Not saved: " + error;
}

void ProfilerWindow::loadFromDialog(){
    nfdu8char_t* outPath = nullptr;
    nfdopendialogu8args_t args = {};
    nfdfilteritem_t filter = {"Profiler capture", "json"};
    args.filterList = &filter;
    args.filterCount = 1;
    args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());
    if (NFD_OpenDialogU8_With(&outPath, &args) != NFD_OKAY)
        return;
    std::string path = outPath;
    NFD_FreePathU8(outPath);
    std::string error;
    if (!loadCapture(path, error))
        statusMessage = "Not loaded: " + error;
}
