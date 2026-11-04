// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Project window's sound commands (spec S36 R19, R27): Import Sound Essentials, Create >
// Placeholder Sound..., and dropping a clip's cached measurements.
#include "ProjectWindow.h"

#include "Backend.h"
#include "Out.h"
#include "audio/AudioAnalysis.h"
#include "audio/AudioImportSettings.h"
#include "audio/AudioProvenance.h"
#include "audio/BuiltinSoundImport.h"
#include "audio/SoundEssentials.h"
#include "audio/SoundSynth.h"
#include "audio/SoundTypes.h"
#include "command/type/SetAssetMetaCmd.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/SHA1.h"
#include "util/Util.h"

#include "imgui.h"

#include <fstream>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

// the dialog's values, kept between its openings
struct PlaceholderDialog {
    int preset = 0;
    int seed = 1;
    int variations = 1;
    float loudness = 0.0f;  // 0: the type's target
    bool useTarget = false;
    std::string error;
};

PlaceholderDialog& placeholderDialog() {
    static PlaceholderDialog dialog;
    return dialog;
}

// The file (or <stem>_i for variations) and its .meta, as generate_sound writes them
bool writePlaceholder(Project* project, const fs::path& directory, const std::string& stem, const PlaceholderDialog& dialog, std::string& error) {
    const std::vector<SoundSynth::Preset>& presets = SoundSynth::presets();
    const SoundSynth::Preset& preset = presets[(size_t)std::clamp(dialog.preset, 0, (int)presets.size() - 1)];
    const SoundTypes::SoundType* type = SoundTypes::find(preset.type);
    const double target = dialog.useTarget ? (double)dialog.loudness : (type ? type->targetLufs : -18.0);
    std::vector<WriteFilesCmd::Item> items;
    for (int i = 1; i <= dialog.variations; i++) {
        const fs::path wav = directory / (dialog.variations == 1 ? stem + ".wav" : stem + "_" + std::to_string(i) + ".wav");
        const uint32_t seed = (uint32_t)dialog.seed + (uint32_t)(i - 1);
        SoundSynth::Output output;
        if (!SoundSynth::generate(preset, seed, nlohmann::json::object(), target, output, error)) return false;
        AudioImportSettings settings;
        settings.type = preset.type;
        AudioProvenance provenance;
        provenance.apply({{"licence", AudioProvenance::LICENCE_GENERATED}, {"source", "generate_sound"},
                          {"generator", {{"preset", preset.name}, {"seed", seed}, {"params", nlohmann::json::object()},
                                         {"loudness_lufs", target}, {"version", SoundSynth::VERSION}}}}, error);
        YAML::Node importSection(YAML::NodeType::Map);
        importSection["audioImporter"] = settings.toYaml();
        YAML::Node sections(YAML::NodeType::Map);
        sections["import"] = importSection;
        sections["provenance"] = provenance.toYaml();
        std::string meta;
        if (!clipMetaText(wav, sections, meta, error)) return false;
        items.push_back({wav, output.wav});
        items.push_back({SetAssetMetaCmd::metaPathOf(wav), meta});
    }
    return ProjectFileWrites::writeUndoable(project, std::move(items), error);
}

} // namespace

void editor::ProjectWindow::assetsImportSoundEssentials() {
    const std::string title = "Import Sound Essentials";
    std::vector<BuiltinSoundImport::Clip> clips;
    std::string error;
    if (!BuiltinSoundImport::resolve({std::string(SoundEssentials::PREFIX) + "*"}, clips, error)) {
        Backend::getApp().registerAlert(title, error);
        return;
    }
    const fs::path directory = project->getAssetsPath() / fs::path(SoundEssentials::IMPORT_FOLDER);
    BuiltinSoundImport::Plan plan;
    if (!BuiltinSoundImport::plan(clips, directory, BuiltinSoundImport::Existing::SkipIdentical, plan, error)) {
        Backend::getApp().registerAlert(title, error);
        return;
    }
    if (!plan.writes.empty()) {
        std::vector<WriteFilesCmd::Item> items;
        for (BuiltinSoundImport::File& file : plan.writes) items.push_back({file.path, std::move(file.content)});
        if (!ProjectFileWrites::writeUndoable(project, std::move(items), error)) {
            Backend::getApp().registerAlert(title, error);
            return;
        }
    }
    std::string message = std::to_string(plan.imported.size()) + " imported into " + SoundEssentials::IMPORT_FOLDER + ", " +
                          std::to_string(plan.skipped.size()) + " already there.";
    if (!plan.conflicts.empty()) {
        message += "\nThese differ from the built-in clip and were left alone:";
        for (const fs::path& file : plan.conflicts) message += "\n  " + project->normalizeToAssetsRelative(file).generic_string();
    }
    Backend::getApp().registerAlert(title, message);
}

void editor::ProjectWindow::assetsCreatePlaceholderSound() {
    placeholderDialog().error.clear();
    openPlaceholderSoundDialog = true;
}

void editor::ProjectWindow::drawPlaceholderSoundDialog() {
    if (openPlaceholderSoundDialog) {
        ImGui::OpenPopup("Placeholder Sound");
        openPlaceholderSoundDialog = false;
    }
    if (!ImGui::BeginPopupModal("Placeholder Sound", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    PlaceholderDialog& dialog = placeholderDialog();
    const std::vector<SoundSynth::Preset>& presets = SoundSynth::presets();
    dialog.preset = std::clamp(dialog.preset, 0, (int)presets.size() - 1);
    if (ImGui::BeginCombo("Preset", presets[(size_t)dialog.preset].name)) {
        for (int i = 0; i < (int)presets.size(); i++) {
            if (ImGui::Selectable(presets[(size_t)i].name, i == dialog.preset)) dialog.preset = i;
        }
        ImGui::EndCombo();
    }
    ImGui::InputInt("Seed", &dialog.seed);
    dialog.seed = std::max(dialog.seed, 0);
    ImGui::SliderInt("Variations", &dialog.variations, 1, 8);
    ImGui::Checkbox("Loudness Target", &dialog.useTarget);
    if (dialog.useTarget) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##target", &dialog.loudness, -40.0f, -6.0f, "%.1f LUFS");
    }
    if (!dialog.error.empty()) ImGui::TextWrapped("%s", dialog.error.c_str());
    if (ImGui::Button("Create")) {
        const fs::path relative = uniqueRelativePath(currentPath, std::string(presets[(size_t)dialog.preset].name), ".wav");
        if (writePlaceholder(project, (project->getProjectPath() / relative).parent_path(), relative.stem().string(), dialog, dialog.error)) {
            scanDirectory(currentPath);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void editor::ProjectWindow::assetsReanalyzeAudio(const std::vector<fs::path>& files) {
    const fs::path cache = AudioAnalysis::cacheDirectory(project->getProjectPath());
    int dropped = 0;
    for (const fs::path& file : files) {
        if (!Util::isAudioFile(file.string())) continue;
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        std::ostringstream bytes;
        bytes << in.rdbuf();
        const std::string sha1 = SHA1::hash(bytes.str());
        std::error_code ec;
        for (fs::directory_iterator it(cache, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().filename().string().rfind(sha1, 0) != 0) continue;
            std::error_code removeError;
            if (fs::remove(it->path(), removeError)) dropped++;
        }
    }
    Out::info("Dropped %d cached audio measurement(s): the next look at the clips measures them again.", dropped);
}
