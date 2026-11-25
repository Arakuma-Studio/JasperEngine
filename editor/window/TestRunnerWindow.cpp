// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "window/TestRunnerWindow.h"

#include "App.h"
#include "Backend.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "mcp/McpServer.h"
#include "texture/TextureData.h"
#include "util/ExternalCodeEditor.h"
#include "util/FileUtils.h"
#include "util/ImageDiff.h"
#include "util/ProjectFileWrites.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using gametest::Json;
namespace fs = std::filesystem;

namespace {

    double runnerWindowNow() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    ImVec4 statusColor(gametest::Status status) {
        switch (status) {
            case gametest::Status::Passed: return ImVec4(0.40f, 0.80f, 0.40f, 1.0f);
            case gametest::Status::Failed: return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
            case gametest::Status::Error: return ImVec4(0.95f, 0.55f, 0.20f, 1.0f);
            case gametest::Status::Hung: return ImVec4(0.85f, 0.35f, 0.85f, 1.0f);
            case gametest::Status::Skipped: return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
        }
        return ImVec4(1, 1, 1, 1);
    }

    const char* statusIcon(gametest::Status status) {
        switch (status) {
            case gametest::Status::Passed: return ICON_FA_CIRCLE_CHECK;
            case gametest::Status::Failed: return ICON_FA_CIRCLE_XMARK;
            case gametest::Status::Error: return ICON_FA_TRIANGLE_EXCLAMATION;
            case gametest::Status::Hung: return ICON_FA_HOURGLASS;
            case gametest::Status::Skipped: return ICON_FA_FORWARD;
        }
        return ICON_FA_CIRCLE;
    }

    std::string modeArg(int target) {
        return target == 1 ? "desktop" : "editor";
    }

}

TestRunnerWindow& TestRunnerWindow::get() {
    static TestRunnerWindow window;
    return window;
}

void TestRunnerWindow::refresh(Project* project, bool force) {
    const double now = runnerWindowNow();
    const bool runChanged = gametests::TestRun::get().revision() != seenRun;
    // a finished run shows its first failure in the detail pane, unless a case is selected
    if (runChanged && selected.empty()) {
        if (const gametests::RunOutcome* last = gametests::TestRun::get().last()) {
            for (const gametest::CaseResult& r : last->result.cases) {
                if (r.status == gametest::Status::Failed || r.status == gametest::Status::Error || r.status == gametest::Status::Hung) {
                    selected = r.spec.id;
                    break;
                }
            }
        }
    }
    if (!force && !runChanged && now - lastScan < 2.0 && project->getProjectPath() == projectDir) return;
    lastScan = now;
    seenRun = gametests::TestRun::get().revision();
    projectDir = project->getProjectPath();
    root = gametests::findRoot(projectDir);
    files.clear();
    settingsError.clear();
    if (root.empty()) return;
    if (!gametests::loadSettings(root, settings, settingsError)) return;
    files = gametests::scan(root, settings);
    std::string note;
    gametests::readLedger(projectDir, ledger, note);
}

const gametest::CaseResult* TestRunnerWindow::resultOf(const std::string& id) const {
    const gametests::RunOutcome* last = gametests::TestRun::get().last();
    if (!last) return nullptr;
    for (const gametest::CaseResult& r : last->result.cases)
        if (r.spec.id == id) return &r;
    return nullptr;
}

bool TestRunnerWindow::passesStatusFilter(const gametest::CaseSpec& spec) const {
    if (filter[0] && !gametest::matchesFilter(spec, filter)) return false;
    const gametest::CaseResult* r = resultOf(spec.id);
    switch (statusFilter) {
        case 1: return r && (r->status == gametest::Status::Failed || r->status == gametest::Status::Error || r->status == gametest::Status::Hung);
        case 2: return r && r->status == gametest::Status::Skipped;
        case 3: {
            const gametests::RunOutcome* last = gametests::TestRun::get().last();
            return last && last->unverified.count(spec.id);
        }
        default: return true;
    }
}

void TestRunnerWindow::startRun(const std::string& runFilter, bool failedOnly, const char* mode) {
    Json args = {{"action", "run"}, {"wait", false}, {"target", modeArg(target)}};
    if (!runFilter.empty()) args["filter"] = runFilter;
    if (failedOnly) args["failed_only"] = true;
    if (mode) args["mode"] = mode;
    message.clear();
    mcp::McpServer::get().runLocal("run_tests", args, "Test Runner", [this](const mcp::ToolResult& result) {
        if (result.isError) message = result.text;
    });
}

void TestRunnerWindow::drawCreateMenu(Project* project) {
    if (!ImGui::BeginPopup("##TestRunnerCreate")) return;
    if (ImGui::MenuItem("Test Folder", nullptr, false, root.empty())) {
        std::vector<std::string> blocking;
        const std::string folder = gametests::folderToCreate(projectDir, blocking);
        std::string error;
        if (!blocking.empty()) {
            std::string list;
            for (const std::string& file : blocking) list += "\n  " + file;
            message = "Tests/ holds assets without tests.yaml:" + list + "\nThe test folder is GameTests/ instead.";
        }
        if (ProjectFileWrites::writeUndoable(project, {{projectDir / folder / gametests::MARKER, gametests::defaultTestsYaml()}}, error, nullptr,
                                             nullptr, {}, false, "Create Test Folder"))
            refresh(project, true);
        else
            message = error;
    }
    if (ImGui::MenuItem("Test Script", nullptr, false, !root.empty())) {
        scriptPopup = true;
        std::strncpy(scriptName, "NewTests", sizeof(scriptName) - 1);
    }
    ImGui::EndPopup();
}

void TestRunnerWindow::drawToolbar(Project* project) {
    gametests::TestRun& run = gametests::TestRun::get();
    const bool running = run.running();
    ImGui::BeginDisabled(running || root.empty() || project->isAnyScenePlaying());
    if (ImGui::Button("Run All")) startRun("", false, nullptr);
    ImGui::SameLine();
    std::string selection;
    for (const auto& [id, on] : checked)
        if (on) selection += (selection.empty() ? "" : ",") + id;
    if (selection.empty() && !selected.empty()) selection = selected;
    ImGui::BeginDisabled(selection.empty());
    if (ImGui::Button("Run Selected")) startRun(selection, false, nullptr);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rerun Failed")) startRun("", true, nullptr);
    ImGui::EndDisabled();
    if (running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) run.cancel();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(130.0f));
    const char* targets[] = {"Editor", "This Computer"};
    ImGui::Combo("##TestRunnerTarget", &target, targets, 2);
    ImGui::SetItemTooltip("Target: the editor's Play sessions, or a Development player of this computer built with Include Tests");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    ImGui::InputTextWithHint("##TestRunnerFilter", "Filter (text or tag:smoke)", filter, sizeof(filter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(130.0f));
    const char* statuses[] = {"All", "Failed", "Skipped", "Unverified On..."};
    ImGui::Combo("##TestRunnerStatus", &statusFilter, statuses, 4);
    ImGui::SameLine();
    if (ImGui::Button("Create")) ImGui::OpenPopup("##TestRunnerCreate");
    drawCreateMenu(project);
}

void TestRunnerWindow::drawTree(Project* project, gametest::Mode mode) {
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##TestRunnerTree", 4, flags, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(110.0f));
    ImGui::TableSetupColumn("Last Passed On", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(160.0f));
    ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(110.0f));
    ImGui::TableHeadersRow();
    for (const gametest::ScannedFile& file : files) {
        std::vector<const gametest::CaseSpec*> cases;
        for (const gametest::CaseSpec& spec : file.cases)
            if (spec.mode == mode && passesStatusFilter(spec)) cases.push_back(&spec);
        if (cases.empty()) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool open = ImGui::TreeNodeEx(file.file.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth);
        if (!open) continue;
        for (const gametest::CaseSpec* spec : cases) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(spec->id.c_str());
            bool on = checked[spec->id];
            if (ImGui::Checkbox("##check", &on)) checked[spec->id] = on;
            ImGui::SameLine();
            const gametest::CaseResult* r = resultOf(spec->id);
            if (r) ImGui::TextColored(statusColor(r->status), "%s", statusIcon(r->status));
            else ImGui::TextDisabled(ICON_FA_CIRCLE);
            ImGui::SameLine();
            if (ImGui::Selectable(spec->name.c_str(), selected == spec->id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                selected = spec->id;
            if (ImGui::BeginPopupContextItem("##caseMenu")) {
                if (ImGui::MenuItem("Run")) startRun(spec->id, false, nullptr);
                if (ImGui::MenuItem("Open Script")) {
                    std::string error;
                    if (!ExternalCodeEditor::open(root / FileUtils::pathFromUtf8(spec->file), spec->line, 1, true, &error)) message = error;
                }
                if (ImGui::MenuItem("Copy ID")) ImGui::SetClipboardText(spec->id.c_str());
                if (ImGui::MenuItem("Reveal Golden")) {
                    const fs::path dir = root / "Golden" / gametest::caseIdPath(spec->id);
                    std::string error;
                    if (!Platform::openWithSystemDefault(FileUtils::pathToUtf8(dir), &error)) message = "No golden folder yet: " + FileUtils::pathToGenericUtf8(dir);
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            if (r && r->status != gametest::Status::Skipped) ImGui::Text("%.2f s (%lld)", r->gameSeconds, static_cast<long long>(r->frames));
            ImGui::TableNextColumn();
            // Last Passed On: a chip per target with a pass, the tooltip its date, backend and revision
            if (ledger.cases.contains(spec->id)) {
                for (auto& [targetName, entry] : ledger.cases[spec->id].items()) {
                    if (!entry.contains("last_pass")) continue;
                    ImGui::SmallButton(targetName.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s\nrevision %s", entry.value("last_pass", "").c_str(), entry.value("backend", "").c_str(),
                                          entry.value("pass_revision", Json::object()).value("id", "").substr(0, 12).c_str());
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
            ImGui::TableNextColumn();
            std::string tags;
            for (const std::string& tag : spec->tags) tags += (tags.empty() ? "" : ", ") + tag;
            ImGui::TextUnformatted(tags.c_str());
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::EndTable();
}

void TestRunnerWindow::preview(Preview& slot, const std::filesystem::path& file, float width) {
    const std::string key = FileUtils::pathToUtf8(file);
    if (slot.path != key) {
        if (!slot.texture.empty()) slot.texture.destroy();
        slot = Preview();
        slot.path = key;
        texsynth::Image loaded;
        std::string error;
        if (texsynth::loadImage(file, loaded, error)) {
            const texsynth::Image rgba = toRgba8Image(loaded);
            const size_t bytes = rgba.data.size();
            void* pixels = std::malloc(bytes);
            if (pixels) {
                std::memcpy(pixels, rgba.data.data(), bytes);
                static uint64_t counter = 0;
                TextureData data(rgba.width, rgba.height, static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
                slot.texture.setData("editor:test-runner:" + std::to_string(++counter), data);
                slot.texture.getRender();
                slot.texture.releaseData();
                slot.width = rgba.width;
                slot.height = rgba.height;
            }
        }
    }
    TextureRender* render = slot.texture.empty() ? nullptr : slot.texture.getRender();
    if (!render || slot.width <= 0) {
        ImGui::TextDisabled("(no image)");
        return;
    }
    const float scale = width / static_cast<float>(slot.width);
    ImGui::Image(Backend::getImGuiTexture(render), ImVec2(width, slot.height * scale));
}

void TestRunnerWindow::releasePreviews() {
    for (Preview* slot : {&golden, &actual, &diff}) {
        if (!slot->texture.empty()) slot->texture.destroy();
        *slot = Preview();
    }
}

void TestRunnerWindow::drawDetail(Project* project) {
    const gametest::CaseResult* r = selected.empty() ? nullptr : resultOf(selected);
    if (!r) {
        ImGui::TextDisabled(selected.empty() ? "Select a case." : "Not run yet.");
        return;
    }
    ImGui::TextColored(statusColor(r->status), "%s %s", statusIcon(r->status), gametest::statusName(r->status));
    ImGui::SameLine();
    ImGui::TextUnformatted(r->spec.id.c_str());
    if (!r->skipReason.empty()) ImGui::TextDisabled("Skipped: %s", r->skipReason.c_str());
    for (size_t i = 0; i < r->failures.size(); i++) {
        const gametest::Failure& f = r->failures[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string label = f.file + ":" + std::to_string(f.line) + ": [" + f.kind + "] " + f.message + " (frame " + std::to_string(f.frame) + ")";
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        // click-to-source: the case file (or the game script a console line names) at its line
        if (ImGui::Selectable(label.c_str())) {
            std::error_code ec;
            fs::path file = root / FileUtils::pathFromUtf8(f.file);
            if (!fs::exists(file, ec)) file = project->getLuaPath() / FileUtils::pathFromUtf8(f.file);
            std::string error;
            if (!ExternalCodeEditor::open(file, std::max(1, f.line), 1, true, &error)) message = error;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    for (const gametest::ConsoleLine& line : r->console)
        ImGui::TextColored(line.level == "error" ? Theme::Colors::ErrorText : Theme::Colors::WarningText, "%s", line.message.c_str());
    for (const std::string& line : r->log) ImGui::TextUnformatted(line.c_str());
    const gametests::RunOutcome* last = gametests::TestRun::get().last();
    for (const Json& capture : r->captures) {
        const std::string name = capture.value("name", std::string());
        const std::string status = capture.value("status", std::string());
        ImGui::Separator();
        ImGui::Text("Capture %s: %s", name.c_str(), status.c_str());
        if (capture.contains("error")) ImGui::TextWrapped("%s", capture.value("error", "").c_str());
        const float width = std::max(Theme::dpi(80.0f), (ImGui::GetContentRegionAvail().x - Theme::dpi(16.0f)) / 3.0f);
        const fs::path resultsDir = gametests::resultsDir(projectDir);
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Golden");
        if (capture.contains("golden") && status != "new") preview(golden, root / FileUtils::pathFromUtf8(capture.value("golden", "")), width);
        else ImGui::TextDisabled("(none yet)");
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Actual");
        if (capture.contains("actual")) preview(actual, resultsDir / FileUtils::pathFromUtf8(capture.value("actual", "")), width);
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Diff");
        if (capture.contains("panel")) preview(diff, resultsDir / FileUtils::pathFromUtf8(capture.value("panel", "")), width);
        else ImGui::TextDisabled("-");
        ImGui::EndGroup();
        if (status == "new" || status == "failed") {
            ImGui::PushID(name.c_str());
            std::vector<std::string> written;
            std::string error;
            if (ImGui::Button("Accept Image")) {
                if (gametests::acceptCaptures(project, {r->spec.id + "/" + name}, false, written, error)) message = "Accepted " + written[0];
                else message = error;
                releasePreviews();
            }
            ImGui::SameLine();
            if (ImGui::Button("Accept For This Target Only")) {
                if (gametests::acceptCaptures(project, {r->spec.id + "/" + name}, true, written, error)) message = "Accepted " + written[0];
                else message = error;
                releasePreviews();
            }
            ImGui::PopID();
        }
    }
    (void)last;
}

void TestRunnerWindow::show(App&, Project* project) {
    if (!windowOpen || !project) return;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(760.0f), Theme::dpi(560.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    refresh(project, false);
    drawToolbar(project);
    if (root.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("No tests yet:");
        ImGui::SameLine();
        if (ImGui::Button("Create Test Folder")) {
            std::vector<std::string> blocking;
            const std::string folder = gametests::folderToCreate(projectDir, blocking);
            std::string error;
            if (!blocking.empty()) message = "Tests/ holds assets without tests.yaml (" + blocking[0] + "...): the test folder is GameTests/.";
            if (!ProjectFileWrites::writeUndoable(project, {{projectDir / folder / gametests::MARKER, gametests::defaultTestsYaml()}}, error, nullptr,
                                                  nullptr, {}, false, "Create Test Folder"))
                message = error;
            refresh(project, true);
        }
    } else if (!settingsError.empty()) {
        ImGui::TextColored(Theme::Colors::ErrorText, "%s", settingsError.c_str());
    } else {
        // counts bar: the last run, and the unverified targets
        const gametests::RunOutcome* last = gametests::TestRun::get().last();
        if (last) {
            const gametest::Counts& c = last->result.counts;
            ImGui::Text("%d passed, %d failed, %d skipped, %d error, %d hung, %zu unverified", c.passed, c.failed, c.skipped, c.error, c.hung,
                        last->unverified.size());
        }
        gametests::TestRun& run = gametests::TestRun::get();
        if (run.running()) {
            const size_t planned = std::max<size_t>(1, run.casesPlanned());
            ImGui::ProgressBar(static_cast<float>(run.casesDone()) / static_cast<float>(planned), ImVec2(-FLT_MIN, 0),
                               (std::to_string(run.casesDone()) + " / " + std::to_string(planned) + "  " + run.currentCase()).c_str());
        }
        if (ImGui::BeginTabBar("##TestRunnerModes")) {
            if (ImGui::BeginTabItem("Edit Mode")) {
                drawTree(project, gametest::Mode::Edit);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Play Mode")) {
                drawTree(project, gametest::Mode::Play);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::BeginChild("##TestRunnerDetail");
        drawDetail(project);
        ImGui::EndChild();
    }
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    if (scriptPopup) {
        ImGui::OpenPopup("Create Test Script##TestRunner");
        scriptPopup = false;
    }
    if (EditorModal::begin("Create Test Script##TestRunner", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorModal::message("A new Tests/<name>.test.lua from the template.");
        EditorModal::inputText("Name", "name", scriptName, sizeof(scriptName));
        if (EditorModal::button("Create", "ok")) {
            std::string start;
            if (const SceneProject* sp = project->getScene(project->getStartSceneId()))
                start = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(sp->filepath));
            const fs::path file = root / (std::string(scriptName) + ".test.lua");
            std::string error;
            if (ProjectFileWrites::writeUndoable(project, {{file, gametests::testScriptTemplate(start)}}, error, nullptr, nullptr, {}, false,
                                                 "Create Test Script " + std::string(scriptName)))
                refresh(project, true);
            else
                message = error;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}
