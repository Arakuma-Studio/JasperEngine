// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's view of an audio file (spec S36 R25; Unity's AudioClip import inspector): the
// waveform or spectrogram with its onsets and peak, the measurements and notes of the clip as it
// plays, Import Settings with draggable handles (Apply and Revert as the Texture Importer's),
// Provenance, labels, and Audition through the engine's own AudioClipSource.
#include "InspectorWindow.h"

#include "Backend.h"
#include "EditorHost.h"
#include "Theme.h"
#include "audio/AudioAnalysis.h"
#include "audio/AudioImages.h"
#include "audio/AudioImportRegistry.h"
#include "audio/AudioImportSettings.h"
#include "audio/AudioMixer.h"
#include "audio/AudioProvenance.h"
#include "audio/AudioWaveform.h"
#include "audio/ClipDefects.h"
#include "audio/ClipSource.h"
#include "audio/SoundTypes.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "external/IconsFontAwesome6.h"
#include "io/Data.h"
#include "audio/AudioFileCheck.h"
#include "jobs/JobSystem.h"
#include "subsystem/AudioSystem.h"
#include "texture/Texture.h"
#include "util/FileUtils.h"

#include "imgui.h"
#include "soloud_wav.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

constexpr float WAVEFORM_HEIGHT = 96.0f;
constexpr float SPECTROGRAM_HEIGHT = 128.0f;
constexpr float HANDLE_HALF_WIDTH = 5.0f;
// what Audition's Match Loudness brings a clip to, momentary max
constexpr double AUDITION_TARGET_LUFS = -20.0;

// One clip view's state: the view shows one asset at a time
struct ClipView {
    fs::path asset;
    // as on disk, and as edited (Apply writes the difference)
    AudioImportSettings stored;
    AudioImportSettings edited;
    AudioProvenance storedProvenance;
    AudioProvenance editedProvenance;
    std::string readError;
    uintmax_t metaStamp = 0;
    bool showSpectrogram = false;
    bool editBuiltin = false;
    // the handle being dragged: 0 none, 1 trim head, 2 trim tail, 3 fade in, 4 fade out, 5 loop start, 6 loop end
    int dragging = 0;

    // the spectrogram, made once per file on a Background job
    struct Picture {
        AudioImages::Image image;
        std::atomic<bool> done{false};
    };
    std::shared_ptr<Picture> picture;
    fs::path pictureFile;
    Texture pictureTexture;
    uint64_t pictureSerial = 0;

    // Audition
    std::shared_ptr<AudioClipSource> voiceClip;
    unsigned int voice = 0;
    bool matchLoudness = false;
};

// never destroyed: its texture would go after the graphics device
ClipView& clipView() {
    static ClipView* view = new ClipView();
    return *view;
}

uintmax_t metaStampOf(const fs::path& asset) {
    std::error_code ec;
    const fs::path meta = SetAssetMetaCmd::metaPathOf(asset);
    const auto time = fs::last_write_time(meta, ec);
    return ec ? 0 : (uintmax_t)time.time_since_epoch().count() ^ fs::file_size(meta, ec);
}

void stopAudition(ClipView& view) {
    if (view.voice != 0 && AudioSystem::isInited()) AudioSystem::getSoloud().stop(view.voice);
    view.voice = 0;
    view.voiceClip.reset();
}

void reloadSections(ClipView& view) {
    view.readError.clear();
    if (!AudioImportSettings::load(view.asset, view.stored, view.readError)) view.stored = AudioImportSettings();
    std::string provenanceError;
    if (!AudioProvenance::load(view.asset, view.storedProvenance, provenanceError)) {
        view.storedProvenance = AudioProvenance();
        if (view.readError.empty()) view.readError = provenanceError;
    }
    view.edited = view.stored;
    view.editedProvenance = view.storedProvenance;
    view.metaStamp = metaStampOf(view.asset);
}

// The clip the engine would play, on a decoded copy of its own (a file edited on disk plays as it now is)
bool startAudition(const Project* project, ClipView& view, double momentaryMax, std::string& error) {
    stopAudition(view);
    if (!AudioSystem::init()) {
        error = "No audio output.";
        return false;
    }
    Data file;
    const std::string path = FileUtils::pathToUtf8(view.asset);
    if (file.open(path.c_str()) != FileErrors::FILEDATA_OK) {
        error = "Can't read the file.";
        return false;
    }
    AudioFileFormat format;
    if (!AudioFileCheck::prepare(file.getMemPtr(), file.length(), AudioDecode::Whole, format, error)) return false;
    auto wav = std::make_shared<SoLoud::Wav>();
    if (wav->loadMem(file.getMemPtr(), (unsigned int)file.length(), false, false) != SoLoud::SO_NO_ERROR) {
        error = "Can't decode the file.";
        return false;
    }
    ClipImportSettings settings;
    if (!view.edited.resolve(format.frames, format.sampleRate, 0.0, 0.0, settings, error)) return false;
    if (view.edited.trimHead.automatic || view.edited.trimTail.automatic) {
        // `auto` trims need the silence: the registry's own resolution of what is stored
        if (!AudioImportRegistry::resolve(*project, view.asset, settings, error)) return false;
    }
    view.voiceClip = std::make_shared<AudioClipSource>(wav, settings);
    float volume = 1.0f;
    if (view.matchLoudness && std::isfinite(momentaryMax)) {
        volume = (float)std::pow(10.0, std::min(AUDITION_TARGET_LUFS - momentaryMax, 24.0) / 20.0);
    }
    view.voice = AudioMixer::play("", *view.voiceClip, false, Vector3(0, 0, 0), volume, false, 0.0);
    if (view.voice == 0) {
        view.voiceClip.reset();
        error = "Could not start a voice.";
        return false;
    }
    return true;
}

void loadSpectrogram(ClipView& view) {
    if (view.pictureFile != view.asset) {
        view.pictureFile = view.asset;
        view.picture.reset();
        view.pictureTexture.destroy();
    }
    if (view.picture) {
        return;
    }
    auto picture = std::make_shared<ClipView::Picture>();
    view.picture = picture;
    const fs::path file = view.asset;
    jobs::submit([picture, file]() {
        AudioAnalysis::Signal signal;
        std::string bytes;
        std::string error;
        Data data;
        if (data.open(FileUtils::pathToUtf8(file).c_str()) == FileErrors::FILEDATA_OK) {
            bytes.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
            if (AudioAnalysis::decodeReleasing(bytes, signal, error)) {
                AudioImages::Input input;
                input.signal = &signal;
                picture->image = AudioImages::render(input, {AudioImages::Lane::Spectrogram}, 768);
            }
        }
        picture->done.store(true);
        getEditorHost().enqueueMainThreadTask([]() {});
    }, {jobs::Lane::Background, jobs::GateClass::Default, "audio-clip-spectrogram"});
}

std::string millisText(double ms) {
    char text[32];
    std::snprintf(text, sizeof(text), ms >= 100.0 ? "%.0f ms" : "%.1f ms", ms);
    return text;
}

struct Numbers {
    double momentaryMax = std::nan("");
    double integrated = std::nan("");
    double truePeak = std::nan("");
    double samplePeak = std::nan("");
    double duration = 0.0;
    double fileDuration = 0.0;
};

double numberOf(const nlohmann::json& parent, const char* group, const char* key) {
    const auto it = parent.find(group);
    if (it == parent.end() || !it->is_object()) return std::nan("");
    const auto value = it->find(key);
    return value != it->end() && value->is_number() ? value->get<double>() : std::nan("");
}

void row(const char* name, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", name);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

std::string dbText(double value, const char* unit) {
    if (!std::isfinite(value)) return "-";
    char text[48];
    std::snprintf(text, sizeof(text), "%.1f %s", value, unit);
    return text;
}

bool textField(const char* label, std::string& value, bool readOnly = false) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-1.0f);
    std::string id = std::string("##") + label;
    const bool changed = ImGui::InputText(id.c_str(), buffer, sizeof(buffer), readOnly ? ImGuiInputTextFlags_ReadOnly : 0);
    if (changed) value = buffer;
    return changed;
}

} // namespace

void editor::InspectorWindow::drawAudioClipAsset(const fs::path& asset) {
    ClipView& view = clipView();
    if (view.asset != asset) {
        stopAudition(view);
        view.pictureTexture.destroy();
        view = ClipView();
        view.asset = asset;
        reloadSections(view);
    } else if (view.metaStamp != metaStampOf(asset) && view.dragging == 0 && view.edited == view.stored &&
               view.editedProvenance.values == view.storedProvenance.values) {
        // an undo, another program, or set_audio_import_settings: what is on disk now
        reloadSections(view);
    }

    drawAssetHeader(ICON_FA_VOLUME_HIGH, asset, "Audio Clip");
    const std::string id = FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(asset));
    const ClipDefects::Result analysis = ClipDefects::get(project, id);
    const AudioWaveform::Result wave = AudioWaveform::get(project, asset);

    Numbers numbers;
    nlohmann::json onsets = nlohmann::json::array();
    double peakMs = -1.0;
    if (analysis.item.is_object()) {
        numbers.momentaryMax = numberOf(analysis.item, "loudness", "momentary_max");
        numbers.integrated = numberOf(analysis.item, "loudness", "integrated_lufs");
        numbers.truePeak = numberOf(analysis.item, "peak", "true_peak_dbtp");
        numbers.samplePeak = numberOf(analysis.item, "peak", "sample_dbfs");
        numbers.fileDuration = numberOf(analysis.item, "format", "duration_s");
        numbers.duration = analysis.item.contains("played") ? numberOf(analysis.item, "played", "duration_s") : numbers.fileDuration;
        onsets = analysis.item.value("onsets_ms", nlohmann::json::array());
        peakMs = numberOf(analysis.item, "onset", "peak_ms");
    }

    // -- the picture
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Button(view.showSpectrogram ? "Waveform" : "Spectrogram")) view.showSpectrogram = !view.showSpectrogram;
    const float width = std::max(64.0f, ImGui::GetContentRegionAvail().x);
    const float height = view.showSpectrogram ? SPECTROGRAM_HEIGHT : WAVEFORM_HEIGHT;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg));
    const double fileMs = wave.data ? wave.data->seconds() * 1000.0 : numbers.fileDuration * 1000.0;
    const auto xOfMs = [&](double ms) { return origin.x + (fileMs > 0.0 ? (float)(ms / fileMs) : 0.0f) * width; };
    const auto msOfX = [&](float x) { return fileMs * (double)std::clamp((x - origin.x) / width, 0.0f, 1.0f); };

    if (view.showSpectrogram) {
        loadSpectrogram(view);
        if (view.picture && view.picture->done && !view.picture->image.rgba.empty() && !view.pictureTexture.getRender()) {
            AudioImages::Image& image = view.picture->image;
            TextureData data(image.width, image.height, (unsigned int)image.rgba.size(), ColorFormat::RGBA, 4, image.rgba.data());
            data.setDataOwned(false);
            view.pictureTexture = Texture("editor-audio-clip-spectrogram-" + std::to_string(++view.pictureSerial), data);
            view.pictureTexture.setMinFilter(TextureFilter::LINEAR);
            view.pictureTexture.setMagFilter(TextureFilter::LINEAR);
            view.pictureTexture.load();
        }
        if (TextureRender* render = view.pictureTexture.getRender()) {
            draw->AddImage(Backend::getImGuiTexture(render), origin, ImVec2(origin.x + width, origin.y + height));
        }
    } else if (wave.data && !wave.data->empty()) {
        const AudioWaveform::Data& data = *wave.data;
        const ImU32 colour = ImGui::GetColorU32(ImGuiCol_PlotLines);
        const float mid = origin.y + height * 0.5f;
        const int columns = std::max(1, (int)width);
        for (int x = 0; x < columns; x++) {
            const int column = std::min(AudioWaveform::COLUMNS - 1, x * AudioWaveform::COLUMNS / columns);
            int low = 32767, high = -32768;
            for (unsigned int c = 0; c < data.channels; c++) {
                low = std::min(low, (int)data.min(c, column));
                high = std::max(high, (int)data.max(c, column));
            }
            draw->AddLine(ImVec2(origin.x + (float)x, mid - (float)high / 32767.0f * height * 0.5f),
                          ImVec2(origin.x + (float)x, mid - (float)low / 32767.0f * height * 0.5f + 1.0f), colour);
        }
    } else if (!wave.error.empty()) {
        draw->AddText(ImVec2(origin.x + 6.0f, origin.y + 6.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), wave.error.c_str());
    }

    // the overlays: the trimmed parts, onsets, the peak, loop flags, fades and handles
    const double headMs = view.edited.trimHead.automatic ? numberOf(analysis.item.is_object() && analysis.item.contains("source") ? analysis.item["source"] : analysis.item, "silence", "head_ms")
                                                          : view.edited.trimHead.ms;
    const double tailMs = view.edited.trimTail.automatic ? numberOf(analysis.item.is_object() && analysis.item.contains("source") ? analysis.item["source"] : analysis.item, "silence", "tail_ms")
                                                          : view.edited.trimTail.ms;
    const double startMs = std::isfinite(headMs) ? headMs : 0.0;
    const double endMs = std::max(startMs, fileMs - (std::isfinite(tailMs) ? tailMs : 0.0));
    if (fileMs > 0.0) {
        const ImU32 shade = IM_COL32(12, 12, 14, 153);
        if (startMs > 0.0) draw->AddRectFilled(origin, ImVec2(xOfMs(startMs), origin.y + height), shade);
        if (endMs < fileMs) draw->AddRectFilled(ImVec2(xOfMs(endMs), origin.y), ImVec2(origin.x + width, origin.y + height), shade);
        const ImU32 onsetColour = IM_COL32(196, 150, 232, 255);
        for (const nlohmann::json& onset : onsets) {
            if (onset.is_number()) draw->AddLine(ImVec2(xOfMs(onset.get<double>() + startMs), origin.y), ImVec2(xOfMs(onset.get<double>() + startMs), origin.y + 8.0f), onsetColour);
        }
        if (peakMs >= 0.0) {
            const float x = xOfMs(peakMs + startMs);
            draw->AddLine(ImVec2(x, origin.y + height - 8.0f), ImVec2(x, origin.y + height), IM_COL32(250, 140, 60, 255));
        }
    }

    struct Handle {
        int id;
        const char* probe;
        double ms;
        bool shown;
        ImU32 colour;
    };
    const ImU32 handleColour = IM_COL32(236, 236, 242, 255);
    const ImU32 loopColour = IM_COL32(110, 200, 110, 255);
    const Handle handles[] = {
        {1, "##audio.handle.trim_head", startMs, true, handleColour},
        {2, "##audio.handle.trim_tail", endMs, true, handleColour},
        {3, "##audio.handle.fade_in", startMs + view.edited.fadeInMs, view.edited.fadeInMs > 0.0, IM_COL32(240, 200, 80, 255)},
        {4, "##audio.handle.fade_out", endMs - view.edited.fadeOutMs, view.edited.fadeOutMs > 0.0, IM_COL32(240, 200, 80, 255)},
        {5, "##audio.handle.loop_start", view.edited.loopStartMs, view.edited.loopStartMs >= 0.0, loopColour},
        {6, "##audio.handle.loop_end", view.edited.loopEndMs, view.edited.loopEndMs >= 0.0, loopColour},
    };
    if (fileMs > 0.0) {
        for (const Handle& handle : handles) {
            if (!handle.shown) continue;
            const float x = xOfMs(handle.ms);
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), handle.colour, 2.0f);
            // whole inside the picture, at the edges too: a trim of 0 still has its grip
            ImGui::SetCursorScreenPos(ImVec2(std::clamp(x - HANDLE_HALF_WIDTH, origin.x, origin.x + width - HANDLE_HALF_WIDTH * 2.0f), origin.y));
            ImGui::InvisibleButton(handle.probe, ImVec2(HANDLE_HALF_WIDTH * 2.0f, height));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive()) {
                view.dragging = handle.id;
                const double ms = std::round(msOfX(ImGui::GetIO().MousePos.x) * 10.0) / 10.0;
                switch (handle.id) {
                    case 1: view.edited.trimHead.automatic = false; view.edited.trimHead.ms = std::clamp(ms, 0.0, endMs); break;
                    case 2: view.edited.trimTail.automatic = false; view.edited.trimTail.ms = std::clamp(fileMs - ms, 0.0, fileMs - startMs); break;
                    case 3: view.edited.fadeInMs = std::clamp(ms - startMs, 0.0, AudioImportSettings::FADE_MAX_MS); break;
                    case 4: view.edited.fadeOutMs = std::clamp(endMs - ms, 0.0, AudioImportSettings::FADE_MAX_MS); break;
                    case 5: view.edited.loopStartMs = std::clamp(ms, startMs, view.edited.loopEndMs >= 0.0 ? view.edited.loopEndMs : endMs); break;
                    default: view.edited.loopEndMs = std::clamp(ms, view.edited.loopStartMs >= 0.0 ? view.edited.loopStartMs : startMs, endMs); break;
                }
            } else if (view.dragging == handle.id) {
                view.dragging = 0;
            }
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(width, 0.0f));

    // -- the measurements and the notes
    if (ImGui::BeginTable("##audio-measures", 2, ImGuiTableFlags_SizingStretchProp)) {
        if (analysis.pending) {
            row("Analysis", "measuring...");
        } else if (analysis.item.is_object()) {
            const nlohmann::json& format = analysis.item["format"];
            row("Format", format.value("codec", std::string()) + " " + format.value("encoding", std::string()) + ", " +
                              std::to_string(format.value("channels", 0)) + " ch, " + std::to_string(format.value("sample_rate", 0)) + " Hz");
            row("Length", millisText(numbers.duration * 1000.0) + (analysis.item.contains("played") ? " of " + millisText(numbers.fileDuration * 1000.0) : std::string()));
            row("Loudness", dbText(numbers.momentaryMax, "LUFS-M") + ", " + dbText(numbers.integrated, "LUFS"));
            row("Peak", dbText(numbers.samplePeak, "dBFS") + ", " + dbText(numbers.truePeak, "dBTP"));
            row("Kind", analysis.item.value("kind", std::string()));
        } else {
            row("Analysis", analysis.error);
        }
        ImGui::EndTable();
    }
    if (analysis.notes.is_array() && !analysis.notes.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        for (const nlohmann::json& note : analysis.notes) {
            ImGui::PushID(note.value("id", std::string()).c_str());
            ImGui::TextColored(Theme::Colors::WarningText, "%s", note.value("text", std::string()).c_str());
            const auto fix = note.find("fix");
            if (fix != note.end() && fix->is_object() && !fix->empty() && ImGui::SmallButton("Add Click-Safe Fades")) {
                ClipDefects::addClickSafeFades(project, asset, *fix);
            }
            ImGui::PopID();
        }
        ImGui::PopTextWrapPos();
    }

    // -- Import Settings
    if (ImGui::CollapsingHeader("Import Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!view.readError.empty()) {
            ImGui::TextColored(Theme::Colors::WarningText, "%s", view.readError.c_str());
        }
        const std::vector<SoundTypes::SoundType>& types = SoundTypes::all();
        const std::string typeLabel = view.edited.type.empty() ? std::string("None") : view.edited.type;
        if (ImGui::BeginCombo("Type", typeLabel.c_str())) {
            if (ImGui::Selectable("None", view.edited.type.empty())) view.edited.type.clear();
            for (const SoundTypes::SoundType& type : types) {
                if (ImGui::Selectable(type.name, view.edited.type == type.name)) view.edited.type = type.name;
            }
            ImGui::EndCombo();
        }
        auto dragField = [](const char* label, double& value, double low, double high, const char* format) {
            ImGui::DragScalar(label, ImGuiDataType_Double, &value, 0.25f, &low, &high, format, ImGuiSliderFlags_AlwaysClamp);
        };
        dragField("Gain", view.edited.gainDb, AudioImportSettings::GAIN_MIN_DB, AudioImportSettings::GAIN_MAX_DB, "%.1f dB");
        auto trimField = [&](const char* label, const char* autoLabel, AudioImportSettings::Trim& trim) {
            bool automatic = trim.automatic;
            if (ImGui::Checkbox(autoLabel, &automatic)) {
                trim.automatic = automatic;
                if (automatic) trim.ms = 0.0;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(trim.automatic);
            dragField(label, trim.ms, 0.0, std::max(fileMs, 1.0), "%.1f ms");
            ImGui::EndDisabled();
        };
        trimField("Trim Head", "Auto##head", view.edited.trimHead);
        trimField("Trim Tail", "Auto##tail", view.edited.trimTail);
        dragField("Fade In", view.edited.fadeInMs, 0.0, AudioImportSettings::FADE_MAX_MS, "%.1f ms");
        dragField("Fade Out", view.edited.fadeOutMs, 0.0, AudioImportSettings::FADE_MAX_MS, "%.1f ms");
        dragField("Loop Start", view.edited.loopStartMs, -1.0, std::max(fileMs, 1.0), view.edited.loopStartMs < 0.0 ? "Trimmed Start" : "%.1f ms");
        dragField("Loop End", view.edited.loopEndMs, -1.0, std::max(fileMs, 1.0), view.edited.loopEndMs < 0.0 ? "Trimmed End" : "%.1f ms");

        std::string problem;
        const bool valid = view.edited.validate(problem);
        if (!valid) ImGui::TextColored(Theme::Colors::WarningText, "%s", problem.c_str());
        const bool changed = !(view.edited == view.stored);
        ImGui::BeginDisabled(!changed || !valid);
        if (ImGui::Button("Apply")) {
            project->refreshAssetDatabase(true);
            auto* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(asset), AudioImportSettings::metaKey, view.edited.toYaml());
            command->setNoMerge();
            if (project->getProjectCommandHistory()->addCommand(command)) reloadSections(view);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) view.edited = view.stored;
        ImGui::EndDisabled();
    }

    // -- Provenance
    if (ImGui::CollapsingHeader("Provenance")) {
        const bool builtin = view.storedProvenance.values.contains("builtin");
        const bool readOnly = builtin && !view.editBuiltin;
        struct Field {
            const char* label;
            const char* key;
        };
        const Field fields[] = {{"Licence", "licence"}, {"Licence URL", "licence_url"}, {"Author", "author"},
                                {"Attribution", "attribution"}, {"Source", "source"}, {"URL", "url"}};
        for (const Field& field : fields) {
            std::string value = view.editedProvenance.text(field.key);
            if (textField(field.label, value, readOnly)) {
                if (value.empty()) view.editedProvenance.values.erase(field.key);
                else view.editedProvenance.values[field.key] = value;
            }
        }
        if (builtin) {
            const nlohmann::json& source = view.storedProvenance.values["builtin"];
            ImGui::TextDisabled("Built-in: %s %s", source.value("set", std::string()).c_str(), source.value("id", std::string()).c_str());
            if (readOnly && ImGui::SmallButton("Edit")) view.editBuiltin = true;
        }
        const bool changed = view.editedProvenance.values != view.storedProvenance.values;
        ImGui::BeginDisabled(!changed);
        if (ImGui::Button("Apply##provenance")) {
            project->refreshAssetDatabase(true);
            auto* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(asset), AudioProvenance::metaKey, view.editedProvenance.toYaml());
            command->setNoMerge();
            if (project->getProjectCommandHistory()->addCommand(command)) reloadSections(view);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert##provenance")) view.editedProvenance = view.storedProvenance;
        ImGui::EndDisabled();
    }

    // -- Labels: edited in the Project window's Asset Labels; shown here
    if (analysis.item.is_object() && analysis.item.contains("labels")) {
        std::string labels;
        for (const nlohmann::json& label : analysis.item["labels"]) labels += (labels.empty() ? "" : ", ") + label.get<std::string>();
        ImGui::TextDisabled("Labels: %s", labels.c_str());
    }

    // -- Audition
    ImGui::Separator();
    const bool auditioning = view.voice != 0 && AudioSystem::isInited() && AudioSystem::getSoloud().isValidVoiceHandle(view.voice);
    if (ImGui::Button(auditioning ? ICON_FA_STOP " Stop" : ICON_FA_PLAY " Audition")) {
        if (auditioning) {
            stopAudition(view);
        } else {
            std::string error;
            if (!startAudition(project, view, numbers.momentaryMax, error)) Backend::getApp().registerAlert("Audition", error);
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Match Loudness", &view.matchLoudness);
    // the button follows the voice to its end
    if (auditioning) getEditorHost().enqueueMainThreadTask([]() {});
}
