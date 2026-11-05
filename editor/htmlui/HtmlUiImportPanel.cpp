// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUiImportPanel.h"

#include "HtmlUiAutoImport.h"
#include "HtmlUiImportMaintenance.h"
#include "ImportManifestWriter.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/UIDocumentSystem.h"
#include "window/ConsoleWindow.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ImportManifest.h"

#include "imgui.h"
#include "util/FileUtils.h"
#include "window/EditorModal.h"

#include <chrono>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
namespace fs = std::filesystem;

namespace {

    // the compat diagnostics of a document, when UIDocumentSystem has them (phase-2 H1b)
    template<typename System>
    auto documentDiagnostics(System* system, Entity entity, int) -> decltype(&system->getDiagnostics(entity)){
        return &system->getDiagnostics(entity);
    }

    template<typename System>
    const std::vector<compat::Diagnostic>* documentDiagnostics(System*, Entity, long){
        return nullptr;
    }

    struct CachedStates {
        std::string document;
        uint32_t loads = 0;
        fs::file_time_type manifestTime{};
        size_t pending = 0;
        std::chrono::steady_clock::time_point computed{};
        std::vector<RemoteResourceState> states;
    };

    std::map<std::pair<uint32_t, Entity>, CachedStates>& cache(){
        static std::map<std::pair<uint32_t, Entity>, CachedStates> states;
        return states;
    }

    const std::vector<RemoteResourceState>& statesFor(const ImportTarget& target, uint32_t sceneId, Entity entity,
                                                      const std::string& document, uint32_t loads){
        CachedStates& cached = cache()[{sceneId, entity}];
        std::error_code ec;
        const fs::file_time_type manifestTime = fs::last_write_time(target.assetsDir / editor::FileUtils::pathFromUtf8(compat::ImportManifest::kPath), ec);
        const size_t pending = HtmlUiAutoImport::get().queue().pending();
        const auto now = std::chrono::steady_clock::now();
        // discovery parses the document: again when something it depends on changed, else once a second
        if (cached.document != document || cached.loads != loads || cached.manifestTime != manifestTime || cached.pending != pending ||
            now - cached.computed > std::chrono::seconds(1)){
            cached.document = document;
            cached.loads = loads;
            cached.manifestTime = manifestTime;
            cached.pending = pending;
            cached.computed = now;
            cached.states = remoteResourceStates(target, document);
        }
        return cached.states;
    }

}

void HtmlUiImportPanel::draw(Project* project, SceneProject* sceneProject, Entity entity, const std::string& documentPath,
                             UIDocumentSystem* documents){
    if (!project || !sceneProject || documentPath.empty())
        return;

    HtmlUiAutoImport& imports = HtmlUiAutoImport::get();
    const ImportTarget target = HtmlUiAutoImport::targetFor(project);
    const uint32_t loads = documents ? documents->getLoadCount(entity) : 0;
    const std::vector<RemoteResourceState>& states = statesFor(target, sceneProject->id, entity, documentPath, loads);

    size_t imported = 0, missing = 0, active = 0, failed = 0;
    for (const RemoteResourceState& state : states){
        const std::optional<ImportStatus> status = imports.queue().status(state.reference.url);
        const bool running = status && (status->state == ImportStatus::State::Queued || status->state == ImportStatus::State::Importing);
        if (running)
            active++;
        else if (state.imported)
            imported++;
        else if (status && status->state == ImportStatus::State::Failed)
            failed++;
        else
            missing++;
    }

    ImGui::SeparatorText("Remote resources");
    if (states.empty()){
        ImGui::TextDisabled("No remote URLs");
    }else{
        std::string summary = std::to_string(imported) + " imported, " + std::to_string(missing + failed) + " missing";
        if (active > 0)
            summary += ", " + std::to_string(active) + " importing";
        ImGui::TextUnformatted(summary.c_str());

        ImGui::BeginDisabled(missing + failed == 0);
        if (ImGui::Button(ICON_FA_DOWNLOAD " Import##htmlui_import")){
            imports.importDocument(project, documentPath, false);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Download the remote resources this document references that are not imported yet into assets/htmlui-imports");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE " Re-import all##htmlui_reimport")){
            imports.importDocument(project, documentPath, true);
        }
        ImGui::SetItemTooltip("Download every remote resource of this document again");
    }
    if (!states.empty())
        ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Remove unused##htmlui_remove_unused")){
        ImGui::OpenPopup("Remove unused imports##htmlui");
    }
    ImGui::SetItemTooltip("Remove the imports no document of the project references, and their files");

    if (EditorModal::begin("Remove unused imports##htmlui", nullptr, ImGuiWindowFlags_AlwaysAutoResize)){
        EditorModal::message("Remove every imported resource no .html/.css file of the project references, and delete its files from assets?");
        ImGui::TextUnformatted("Remove every imported resource no .html/.css file of the project references,\nand delete its files from assets?");
        ImGui::Separator();
        if (EditorModal::button("Remove", "yes", ImVec2(120, 0))){
            const RemoveUnusedResult result = removeUnusedImports(target);
            if (!result.error.empty()){
                Out::error("[HTML UI] Remove unused imports failed: %s", result.error.c_str());
            }else{
                for (const std::string& url : result.removedUrls)
                    Out::info("[HTML UI] removed the unused import %s", url.c_str());
                Out::info("[HTML UI] %zu unused import%s removed, %zu file%s deleted", result.removedUrls.size(),
                          result.removedUrls.size() == 1 ? "" : "s", result.deletedFiles.size(), result.deletedFiles.size() == 1 ? "" : "s");
                if (!result.removedUrls.empty()){
                    compat::ImportManifest::reload();
                }
            }
            cache().clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!states.empty()){
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        const float height = std::min((float)states.size(), 6.0f) * lineHeight + ImGui::GetStyle().WindowPadding.y * 2.0f;
        ImGui::BeginChild("##htmlui_remote_resources", ImVec2(0, height), ImGuiChildFlags_Borders);
        for (const RemoteResourceState& state : states){
            const std::optional<ImportStatus> status = imports.queue().status(state.reference.url);
            const char* icon = ICON_FA_CIRCLE_XMARK;
            ImVec4 color = Theme::Colors::ErrorText;
            std::string tooltip;
            if (status && status->state == ImportStatus::State::Queued){
                icon = ICON_FA_CLOCK;
                color = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                tooltip = "Queued for import";
            }else if (status && status->state == ImportStatus::State::Importing){
                icon = ICON_FA_SPINNER;
                color = ImGui::GetStyle().Colors[ImGuiCol_Text];
                tooltip = "Importing...";
            }else if (state.imported){
                icon = ICON_FA_CIRCLE_CHECK;
                color = ImGui::GetStyle().Colors[ImGuiCol_Text];
                tooltip = "Imported as " + target.toProjectRelative(state.localPath) + "\nLicense: " +
                          (state.licenseName.empty() ? std::string("unknown") : state.licenseName);
            }else if (state.fileMissing){
                icon = ICON_FA_TRIANGLE_EXCLAMATION;
                color = Theme::Colors::WarningText;
                tooltip = "The manifest entry's file " + target.toProjectRelative(state.localPath) + " is missing: re-import";
            }else if (status && status->state == ImportStatus::State::Failed){
                tooltip = "Import failed: " + status->error + "\n(Import retries)";
            }else{
                tooltip = "Not imported: the runtime skips it (HTMLUI-IMP-0001)";
            }
            if (state.reference.location.line > 0)
                tooltip += "\n" + state.reference.location.file + ":" + std::to_string(state.reference.location.line) + ":" +
                           std::to_string(state.reference.location.column);

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(icon);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(state.reference.url.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip.c_str());
        }
        ImGui::EndChild();
    }

    // the compat layer's report for this document
    if (const std::vector<compat::Diagnostic>* diagnostics = documents ? documentDiagnostics(documents, entity, 0) : nullptr){
        size_t errors = 0, warnings = 0, notes = 0;
        for (const compat::Diagnostic& diagnostic : *diagnostics){
            if (diagnostic.severity == compat::Severity::Error)
                errors++;
            else if (diagnostic.severity == compat::Severity::Warning)
                warnings++;
            else
                notes++;
        }
        ImGui::SeparatorText("Diagnostics");
        const std::string text = std::to_string(errors) + (errors == 1 ? " error, " : " errors, ") + std::to_string(warnings) +
                                 (warnings == 1 ? " warning, " : " warnings, ") + std::to_string(notes) + (notes == 1 ? " note" : " notes");
        if (errors > 0)
            ImGui::TextColored(Theme::Colors::ErrorText, "%s", text.c_str());
        else if (warnings > 0)
            ImGui::TextColored(Theme::Colors::WarningText, "%s", text.c_str());
        else
            ImGui::TextUnformatted(text.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(diagnostics->empty());
        if (ImGui::Button(ICON_FA_TERMINAL " Show in Console##htmlui_diagnostics")){
            compat::logDiagnostics(documentPath, *diagnostics, true);
            ImGui::SetWindowFocus(ConsoleWindow::WINDOW_NAME);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Print every diagnostic of the document (notes included) to the Console");
    }
}
