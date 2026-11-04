// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FrameDebuggerWindow.h"

#include "App.h"
#include "AppSettings.h"
#include "Backend.h"
#include "Project.h"
#include "Theme.h"
#include "render/RenderUtil.h"
#include "render/SystemRender.h"
#include "render/DrawBatcher.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* const MODES[] = {"Color", "Alpha", "Depth", "R", "G", "B"};

    const char* loadName(PassLoadAction action){
        return action == PassLoadAction::Load ? "Load" : (action == PassLoadAction::DontCare ? "Don't care" : "Clear");
    }

    const char* storeName(PassStoreAction action){
        return action == PassStoreAction::Store ? "Store" : "Don't care";
    }

    std::string attachmentText(const PassAttachment& attachment){
        char text[160];
        std::snprintf(text, sizeof(text), "%s  %dx%d %s%s", attachment.label[0] ? attachment.label : "(unnamed)", attachment.width,
                      attachment.height, attachment.format, attachment.samples > 1 ? (" msaa " + std::to_string(attachment.samples)).c_str() : "");
        std::string out = text;
        if (attachment.mip)
            out += "  mip " + std::to_string(attachment.mip);
        if (attachment.cube)
            out += "  face " + std::to_string(attachment.slice);
        else if (attachment.array)
            out += "  slice " + std::to_string(attachment.slice);
        return out;
    }

    // a fold of the pass list: consecutive passes with the same label and size
    bool sameGroup(const PassRecord& a, const PassRecord& b){
        return std::strcmp(a.label, b.label) == 0 && a.width == b.width && a.height == b.height;
    }

}

FrameDebuggerWindow::FrameDebuggerWindow(App* app): app(app){
}

void FrameDebuggerWindow::setOpen(bool open){
    if (open && !windowOpen)
        focusRequested = true;
    windowOpen = open;
}

void FrameDebuggerWindow::setEnabled(bool on){
    enabled = on;
    if (!on){
        SystemRender::cancelPassPreview();
        if (drawList)
            SystemRender::setDrawListEnabled(false);
    }else if (drawList){
        SystemRender::setDrawListEnabled(true);
    }
}

std::string FrameDebuggerWindow::viewName(uint32_t tag) const{
    const bool playing = app && app->getProject() && app->getProject()->isPlaySessionActive();
    if (tag == SystemRender::FRAME_STATS_FRAME)
        return playing ? "Game" : "Scene";
    if (tag == GAME_VIEW_ID)
        return "Game";
    if (tag == SCENE_VIEW_ID)
        return playing ? "Scene" : "Scene overlays";
    return "View " + std::to_string(tag);
}

void FrameDebuggerWindow::show(){
    windowFocused = false;
    if (!windowOpen){
        if (enabled)
            setEnabled(false);
        return;
    }
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(900.0f, 560.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)){
        ImGui::End();
        return;
    }
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    mode = std::clamp(AppSettings::getProfilerSettings().debuggerMode, 0, 5);

    // the toolbar
    bool live = enabled;
    if (ImGui::Checkbox("Enable", &live))
        setEnabled(live);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the frame's passes live (the view redraws every frame)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!enabled);
    ImGui::Checkbox("Freeze", &frozen);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hold the last captured frame");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(150.0f));
    if (ImGui::BeginCombo("##View", viewName(viewTag).c_str())){
        std::vector<uint32_t> tags = SystemRender::getRecordedViewTags();
        if (std::find(tags.begin(), tags.end(), SystemRender::FRAME_STATS_FRAME) == tags.end())
            tags.insert(tags.begin(), SystemRender::FRAME_STATS_FRAME);
        for (uint32_t tag : tags){
            if (ImGui::Selectable(viewName(tag).c_str(), tag == viewTag)){
                viewTag = tag;
                selectedPass = -1;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Draw list", &drawList))
        SystemRender::setDrawListEnabled(drawList && enabled);

    if (enabled){
        if (app)
            app->requestContinuousFrames(0.5);
        // GPU times for the passes, a few frames late
        SystemRender::leaseGpuTiming(1.0);
        if (!frozen)
            passes = SystemRender::getPassList(viewTag, SystemRender::getGpuTimingState().available);
    }

    ImGui::Separator();
    if (!enabled && passes.passes.empty()){
        ImGui::TextDisabled("Enable to list the passes of the %s view's frame.", viewName(viewTag).c_str());
        ImGui::End();
        return;
    }
    if (passes.passes.empty()){
        ImGui::TextDisabled("The %s view drew nothing in the last frames (is it shown?).", viewName(viewTag).c_str());
        ImGui::End();
        return;
    }
    ImGui::Text("Frame %llu: %zu passes in %zu commit(s)%s", (unsigned long long)passes.frame, passes.passes.size(), passes.commits.size(),
                passes.gpuFrameValid ? ("  GPU " + std::to_string(passes.gpuFrameMs).substr(0, 6) + " ms").c_str() : "");

    if (ImGui::BeginTable("##FrameDebugger", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)){
        ImGui::TableSetupColumn("Passes", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(320.0f));
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##PassTree", ImVec2(0.0f, 0.0f));
        drawPassTree();
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("##PassDetail", ImVec2(0.0f, 0.0f));
        if (selectedPass >= 0 && selectedPass < (int)passes.passes.size())
            drawPassDetail(passes.passes[(size_t)selectedPass]);
        else
            ImGui::TextDisabled("Select a pass.");
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

void FrameDebuggerWindow::drawPassTree(){
    const std::vector<PassRecord>& list = passes.passes;
    for (size_t i = 0; i < list.size();){
        size_t count = 1;
        while (i + count < list.size() && sameGroup(list[i], list[i + count]))
            count++;
        auto passRow = [&](size_t index, const char* text){
            const PassRecord& pass = list[index];
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
            const bool hasDraws = pass.numDraws > 0;
            if (!hasDraws)
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if ((int)index == selectedPass)
                flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx((void*)(intptr_t)(index + 1), flags, "%s", text);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                selectedPass = (int)index;
            if (!pass.executed){
                ImGui::SameLine();
                ImGui::TextDisabled("(skipped)");
            }else if (pass.gpuValid){
                ImGui::SameLine();
                ImGui::TextDisabled("%.3f ms", pass.gpuMs);
            }
            if (hasDraws && open){
                for (uint32_t d = 0; d < pass.numDraws && pass.firstDraw + d < passes.draws.size(); d++){
                    const DrawRecord& draw = passes.draws[pass.firstDraw + d];
                    if (draw.batchItems > 0){
                        // Dynamic Batching: the objects of the draw, and (Unity's Frame Debugger) why its
                        // first one was not batched with the draw before it
                        ImGui::BulletText("Dynamic Batch (%u objects)  %s  %u elements", (unsigned int)draw.batchItems,
                                          draw.shader[0] ? draw.shader : draw.pipeline, draw.elements);
                        if (draw.batchBreak != 0 && ImGui::IsItemHovered()){
                            ImGui::SetTooltip("Why this draw call can't be batched with the previous one: %s",
                                              getBatchBreakName((BatchBreak)draw.batchBreak));
                        }
                    }else{
                        ImGui::BulletText("%s  %u elements x%u", draw.shader[0] ? draw.shader : draw.pipeline, draw.elements, draw.instances);
                    }
                }
                ImGui::TreePop();
            }
        };
        char text[160];
        if (count == 1){
            std::snprintf(text, sizeof(text), "%u  %s %dx%d", list[i].index, list[i].label, list[i].width, list[i].height);
            passRow(i, text);
        }else{
            std::snprintf(text, sizeof(text), "%s %dx%d x%zu", list[i].label, list[i].width, list[i].height, count);
            if (ImGui::TreeNodeEx((void*)(intptr_t)(100000 + i), ImGuiTreeNodeFlags_SpanAvailWidth, "%s", text)){
                for (size_t k = 0; k < count; k++){
                    std::snprintf(text, sizeof(text), "%u  %s #%zu", list[i + k].index, list[i + k].label, k + 1);
                    passRow(i + k, text);
                }
                ImGui::TreePop();
            }
        }
        i += count;
    }
}

void FrameDebuggerWindow::drawPassDetail(const PassRecord& pass){
    ImGui::Text("%s", pass.label);
    ImGui::TextDisabled("Pass %u of the %s view, commit %llu%s", pass.index, viewName(passes.viewTag).c_str(),
                        (unsigned long long)pass.commit, pass.executed ? "" : " (skipped by sokol)");
    if (ImGui::BeginTable("##Targets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)){
        ImGui::TableSetupColumn("Attachment", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(80.0f));
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Load / Store", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(150.0f));
        ImGui::TableHeadersRow();
        auto row = [&](const char* slot, const std::string& target, const std::string& actions){
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(slot);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(target.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(actions.c_str());
        };
        if (pass.swapchain)
            row("Swapchain", std::to_string(pass.width) + "x" + std::to_string(pass.height),
                std::string(loadName(pass.colorLoad[0])) + " / " + storeName(pass.colorStore[0]));
        for (int i = 0; i < pass.numColors; i++){
            if (!pass.colors[i].used)
                continue;
            row(("Color " + std::to_string(i)).c_str(), attachmentText(pass.colors[i]),
                std::string(loadName(pass.colorLoad[i])) + " / " + storeName(pass.colorStore[i]));
            if (pass.resolves[i].used)
                row(("Resolve " + std::to_string(i)).c_str(), attachmentText(pass.resolves[i]), "");
        }
        if (pass.depth.used)
            row("Depth", attachmentText(pass.depth), std::string(loadName(pass.depthLoad)) + " / " + storeName(pass.depthStore));
        ImGui::EndTable();
    }
    ImGui::Text("Draws %u   Instances %u   Triangles %llu   Elements %llu", pass.draws, pass.instances,
                (unsigned long long)pass.triangles, (unsigned long long)pass.elements);
    ImGui::Text("Pipelines %u   Bindings %u   Uniforms %u bytes", pass.pipelines, pass.bindings, pass.uniformBytes);
    if (pass.gpuValid)
        ImGui::Text("CPU %.3f ms   GPU %.4f ms", pass.cpuMs, pass.gpuMs);
    else
        ImGui::Text("CPU %.3f ms   GPU %s", pass.cpuMs, pass.gpuPending ? "pending" : "n/a");
    ImGui::Separator();
    drawPreview(pass);
}

void FrameDebuggerWindow::drawPreview(const PassRecord& pass){
    if (pass.swapchain){
        ImGui::TextDisabled("Swapchain pass: it draws into the window, which has no image to preview.");
        return;
    }
    // the attachments there are
    std::vector<std::pair<int, std::string>> choices;
    for (int i = 0; i < pass.numColors; i++){
        if (pass.colors[i].used)
            choices.emplace_back(i, "Color " + std::to_string(i));
    }
    if (pass.depth.used)
        choices.emplace_back(4, "Depth");
    if (choices.empty()){
        ImGui::TextDisabled("No attachment.");
        return;
    }
    if (std::none_of(choices.begin(), choices.end(), [&](const auto& c){ return c.first == attachment; })){
        attachment = choices.front().first;
        if (attachment == 4 && mode < 2)
            mode = 2;
    }
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    std::string current;
    for (const auto& choice : choices){
        if (choice.first == attachment)
            current = choice.second;
    }
    if (ImGui::BeginCombo("##Attachment", current.c_str())){
        for (const auto& choice : choices){
            if (ImGui::Selectable(choice.second.c_str(), choice.first == attachment)){
                attachment = choice.first;
                mode = attachment == 4 ? 2 : (mode == 2 ? 0 : mode);
            }
        }
        ImGui::EndCombo();
    }
    for (int m = 0; m < 6; m++){
        ImGui::SameLine();
        if (ImGui::RadioButton(MODES[m], mode == m)){
            mode = m;
            ProfilerSettings settings = AppSettings::getProfilerSettings();
            settings.debuggerMode = m;
            AppSettings::setProfilerSettings(settings);
        }
    }
    if (mode == 2){
        ImGui::Checkbox("Linear (near/far)", &linearDepth);
        if (linearDepth){
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Theme::dpi(160.0f));
            ImGui::DragFloatRange2("##NearFar", &nearPlane, &farPlane, 0.1f, 0.0001f, 100000.0f, "near %.3f", "far %.1f");
        }else{
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Theme::dpi(220.0f));
            ImGui::DragFloatRange2("Levels", &depthMin, &depthMax, 0.001f, 0.0f, 1.0f, "%.3f", "%.3f");
        }
    }

    // which pass of that label it is, for the request
    uint32_t occurrence = 0;
    for (size_t i = 0; i < passes.passes.size() && passes.passes[i].index < pass.index; i++){
        if (std::strcmp(passes.passes[i].label, pass.label) == 0)
            occurrence++;
    }
    const auto now = std::chrono::steady_clock::now();
    if (enabled && !frozen && now - lastPreview > std::chrono::milliseconds(100)){
        PassPreviewRequest request;
        request.viewTag = passes.viewTag;
        request.label = pass.label;
        request.occurrence = occurrence;
        request.attachment = attachment;
        request.mode = mode;
        request.nearPlane = linearDepth ? nearPlane : 0.0f;
        request.farPlane = linearDepth ? farPlane : 0.0f;
        request.depthMin = depthMin;
        request.depthMax = depthMax;
        SystemRender::requestPassPreview(request);
        lastPreview = now;
    }
    const PassPreviewResult result = SystemRender::getPassPreview();
    const bool matches = result.request.label == pass.label && result.request.occurrence == occurrence &&
                         result.request.viewTag == passes.viewTag && result.request.attachment == attachment;
    if (!matches){
        ImGui::TextDisabled(enabled ? "Preparing the preview..." : "Enable to preview the pass's output.");
        return;
    }
    if (result.unavailable){
        ImGui::TextColored(Theme::Colors::WarningText, "Preview unavailable: %s", result.reason.c_str());
        return;
    }
    TextureRender* texture = SystemRender::getPassPreviewTexture();
    if (!result.ready || !texture){
        ImGui::TextDisabled("Preparing the preview...");
        return;
    }
    ImGui::TextDisabled("%dx%d %s%s, shown at %dx%d (frame %llu)", result.sourceWidth, result.sourceHeight, result.format.c_str(),
                        result.resolved ? " (resolve image)" : "", result.width, result.height, (unsigned long long)result.frame);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float scale = std::min(available.x / (float)result.width, std::max(available.y, Theme::dpi(64.0f)) / (float)result.height);
    const ImVec2 size((float)result.width * std::min(scale, 1.0f), (float)result.height * std::min(scale, 1.0f));
    ImGui::Image(Backend::getImGuiTexture(texture), size);
}
