// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundBrowserWindow.h"

#include "AppSettings.h"
#include "Backend.h"
#include "Project.h"
#include "Theme.h"
#include "audio/AudioMeter.h"
#include "audio/AudioWaveform.h"
#include "audio/BuiltinSoundImport.h"
#include "audio/SoundDrop.h"
#include "audio/SoundEssentials.h"
#include "audio/SoundEventDocument.h"
#include "audio/SoundEventRender.h"
#include "audio/SoundLibrary.h"
#include "audio/SoundTypes.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/AudioSystem.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include "imgui.h"
#include "soloud.h"
#include "soloud_wav.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    constexpr float CARD_WIDTH = 220.0f;
    constexpr float CARD_HEIGHT = 96.0f;
    constexpr double MATCHED_LUFS = -20.0;
    constexpr int WAVE_COLUMNS = 96;

    std::string builtinReference(const SoundSearch::Entry& entry) {
        return entry.path;
    }

    // a level per column, for a card: the channels' largest peak over each span
    std::shared_ptr<std::vector<float>> waveLevels(const AudioWaveform::Data& data) {
        auto levels = std::make_shared<std::vector<float>>(WAVE_COLUMNS, 0.0f);
        if (data.empty()) return levels;
        for (int x = 0; x < WAVE_COLUMNS; x++) {
            const int from = x * AudioWaveform::COLUMNS / WAVE_COLUMNS;
            const int to = std::max(from + 1, (x + 1) * AudioWaveform::COLUMNS / WAVE_COLUMNS);
            float peak = 0.0f;
            for (int column = from; column < to; column++) {
                for (unsigned int channel = 0; channel < data.channels; channel++) {
                    peak = std::max(peak, std::fabs((float)data.min(channel, column)));
                    peak = std::max(peak, std::fabs((float)data.max(channel, column)));
                }
            }
            (*levels)[x] = peak / 32767.0f;
        }
        return levels;
    }

    bool readFileBytes(const fs::path& file, std::string& bytes) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !bytes.empty();
    }

}

SoundBrowserWindow::SoundBrowserWindow(Project* project): project(project) {
}

SoundBrowserWindow::~SoundBrowserWindow() {
    if (indexJob) indexJob->cancel = true;
    stopPreview();
}

void SoundBrowserWindow::open() {
    windowOpen = true;
    focusRequested = true;
    indexStale = true;
}

void SoundBrowserWindow::setOpen(bool open) {
    windowOpen = open;
    if (open) indexStale = true;
}

bool SoundBrowserWindow::isOpen() const {
    return windowOpen;
}

bool SoundBrowserWindow::isFocused() const {
    return windowOpen && focused;
}

void SoundBrowserWindow::resetForProjectSwitch() {
    if (indexJob) indexJob->cancel = true;
    indexJob.reset();
    stopPreview();
    indexEntries.clear();
    results.clear();
    selection.clear();
    cursor = -1;
    indexStale = true;
    AudioWaveform::clear();
}

void SoundBrowserWindow::startIndex() {
    indexStale = false;
    if (indexJob) indexJob->cancel = true;
    auto job = std::make_shared<IndexJob>();
    job->sources = SoundSearch::sources(project, scope);
    cacheDirectory = job->sources.cacheDirectory;
    indexJob = job;
    std::thread([job]() {
        int analysed = 0;
        job->entries = SoundSearch::index(job->sources, analysed, &job->cancel);
        job->done = true;
    }).detach();
}

void SoundBrowserWindow::runSearch() {
    resultsStale = false;
    const std::string keep = cursor >= 0 && cursor < (int)results.size() ? results[cursor].path : std::string();
    SoundSearch::Query query;
    query.text = searchText;
    query.type = typeFilter;
    query.scope = scope;
    query.filters = filters;
    query.similarTo = similarTo;
    query.limit = SoundSearch::EVERY_MATCH;
    searchError.clear();
    if (!SoundSearch::search(indexEntries, query, results, totalResults, searchError)) results.clear();
    hint = results.empty() && searchError.empty() ? SoundSearch::emptyHint(indexEntries, query) : SoundSearch::Hint();
    selection.clear();
    cursor = -1;
    for (int i = 0; i < (int)results.size(); i++) {
        if (results[i].path == keep) {
            cursor = i;
            selection.insert(i);
            break;
        }
    }
}

const SoundSearch::Entry* SoundBrowserWindow::selectedEntry() const {
    return cursor >= 0 && cursor < (int)results.size() ? &results[cursor] : nullptr;
}

bool SoundBrowserWindow::hasSelection() const {
    return selectedEntry() != nullptr;
}

// ---------------------------------------------------------------- commands

void SoundBrowserWindow::focusSearch() {
    searchFocusRequested = true;
}

void SoundBrowserWindow::select(int index, bool additive) {
    if (index < 0 || index >= (int)results.size()) return;
    if (!additive) selection.clear();
    selection.insert(index);
    cursor = index;
    scrollToCursor = true;
    const SoundSearch::Entry& entry = results[index];
    if (entry.source == "project") project->setInspectedAsset(entry.file);
    if (AppSettings::getSoundBrowserAutoplay()) playOrStop(false);
}

void SoundBrowserWindow::selectNext(int step) {
    if (results.empty()) return;
    const int next = cursor < 0 ? 0 : std::clamp(cursor + step, 0, (int)results.size() - 1);
    const bool playing = previewHandle != 0 || previewJob != nullptr;
    select(next, false);
    // walking the list goes on hearing it
    if (playing && !AppSettings::getSoundBrowserAutoplay()) playOrStop(false);
}

void SoundBrowserWindow::playOrStop(bool matched) {
    const SoundSearch::Entry* entry = selectedEntry();
    if (!entry) return;
    const bool same = previewPath == entry->path && (previewHandle != 0 || previewJob != nullptr);
    if (same && !matched && !AppSettings::getSoundBrowserAutoplay()) {
        stopPreview();
        return;
    }
    const bool match = matched || AppSettings::getSoundBrowserMatchLoudness();
    const double gain = match && std::isfinite(entry->lufs) ? std::clamp(MATCHED_LUFS - (entry->lufs + entry->gainDb), -40.0, 24.0) : 0.0;
    startPreview(*entry, gain);
}

void SoundBrowserWindow::useSelected() {
    const SoundSearch::Entry* entry = selectedEntry();
    if (!entry) return;
    if (entry->source == "project") {
        project->setInspectedAsset(entry->file);
        return;
    }
    std::vector<std::string> references{builtinReference(*entry)};
    std::vector<BuiltinSoundImport::Clip> clips;
    std::string error;
    if (!BuiltinSoundImport::resolve(references, clips, error)) {
        Backend::getApp().registerAlert("Import Sound", error);
        return;
    }
    const char* folder = clips.front().library ? SoundLibrary::IMPORT_FOLDER : SoundEssentials::IMPORT_FOLDER;
    BuiltinSoundImport::Plan plan;
    if (!BuiltinSoundImport::plan(clips, project->getAssetsPath() / fs::path(folder), BuiltinSoundImport::Existing::SkipIdentical, plan, error)) {
        Backend::getApp().registerAlert("Import Sound", error);
        return;
    }
    if (!plan.writes.empty()) {
        std::vector<WriteFilesCmd::Item> items;
        for (BuiltinSoundImport::File& file : plan.writes) items.push_back({file.path, std::move(file.content)});
        if (!ProjectFileWrites::writeUndoable(project, std::move(items), error)) {
            Backend::getApp().registerAlert("Import Sound", error);
            return;
        }
    }
    if (!plan.imported.empty()) project->setInspectedAsset(plan.imported.front());
    else if (!plan.skipped.empty()) project->setInspectedAsset(plan.skipped.front());
    results[cursor].inProject = true;
}

void SoundBrowserWindow::selectAll() {
    selection.clear();
    for (int i = 0; i < (int)results.size(); i++) selection.insert(i);
}

void SoundBrowserWindow::stopOrClear() {
    if (previewHandle != 0 || previewJob) {
        stopPreview();
        return;
    }
    if (searchText[0] != '\0') {
        searchText[0] = '\0';
        resultsStale = true;
    }
}

// ---------------------------------------------------------------- previews

void SoundBrowserWindow::stopPreview() {
    previewJob.reset();
    if (previewHandle != 0 && AudioSystem::isInited()) AudioSystem::getSoloud().stop(previewHandle);
    previewHandle = 0;
    previewPath.clear();
    previewSample.reset();
}

void SoundBrowserWindow::startPreview(const SoundSearch::Entry& entry, double gainDb) {
    stopPreview();
    // nothing of an audition may reach a capture of the game's sound
    if (AudioSystem::isDeterministicHeld()) return;
    previewPath = entry.path;
    previewGainDb = gainDb;
    auto job = std::make_shared<PreviewJob>();
    job->entry = entry;
    previewJob = job;
    if (entry.source == "project" && entry.kind == "event") {
        // one play of the event, as Audition renders it
        SoundEventDef def;
        std::string error;
        if (!SoundEventDocument::read(entry.file, def, &error)) {
            job->error = error;
            job->done = true;
            return;
        }
        const SoundEventRender::Input input = SoundEventRender::prepare(*project, def);
        const uint64_t seed = (uint64_t)project->getAudioSettings().seed;
        const std::string key = entry.guid.empty() ? entry.id : entry.guid;
        std::thread([job, def, input, seed, key]() {
            std::vector<SoundEventRender::Rendition> renditions;
            if (SoundEventRender::render(def, key, input, 1, seed, renditions, job->error) && !renditions.empty()) {
                job->bytes.assign(reinterpret_cast<const char*>(renditions[0].samples.data()), renditions[0].samples.size() * sizeof(float));
            }
            job->done = true;
        }).detach();
        return;
    }
    std::thread([job]() {
        const SoundSearch::Entry& clip = job->entry;
        if (clip.source == "project") {
            if (!readFileBytes(clip.file, job->bytes)) job->error = "the file does not read";
        } else {
            std::vector<BuiltinSoundImport::Clip> clips;
            if (BuiltinSoundImport::resolve({builtinReference(clip)}, clips, job->error)) clips.front().read(job->bytes, job->error);
        }
        job->done = true;
    }).detach();
}

void SoundBrowserWindow::updatePreview() {
    if (previewJob && previewJob->done) {
        const std::shared_ptr<PreviewJob> job = previewJob;
        previewJob.reset();
        if (job->bytes.empty() || !AudioSystem::init()) {
            previewPath.clear();
            return;
        }
        auto wav = std::make_shared<SoLoud::Wav>();
        const bool rendered = job->entry.kind == "event";
        SoLoud::result loaded = SoLoud::SO_NO_ERROR;
        if (rendered) {
            // SoLoud's raw float wave is one buffer per channel
            const size_t count = job->bytes.size() / sizeof(float);
            const float* interleaved = reinterpret_cast<const float*>(job->bytes.data());
            std::vector<float> planar(count);
            const size_t frames = count / SoundEventRender::CHANNELS;
            for (size_t f = 0; f < frames; f++) {
                planar[f] = interleaved[f * 2];
                planar[frames + f] = interleaved[f * 2 + 1];
            }
            loaded = wav->loadRawWave(planar.data(), (unsigned int)count, (float)SoundEventRender::SAMPLE_RATE, SoundEventRender::CHANNELS, true, false);
        } else {
            loaded = wav->loadMem(reinterpret_cast<const unsigned char*>(job->bytes.data()), (unsigned int)job->bytes.size(), true, false);
        }
        if (loaded != SoLoud::SO_NO_ERROR) {
            previewPath.clear();
            return;
        }
        previewSample = wav;
        previewHandle = AudioSystem::getSoloud().play(*wav, (float)std::pow(10.0, previewGainDb / 20.0));
    }
    if (previewHandle != 0 && (!AudioSystem::isInited() || !AudioSystem::getSoloud().isValidVoiceHandle(previewHandle))) {
        previewHandle = 0;
        previewPath.clear();
        previewSample.reset();
    }
    if (previewHandle != 0 && AudioSystem::isDeterministicHeld()) stopPreview();
}

// ---------------------------------------------------------------- drawing

void SoundBrowserWindow::drawToolbar() {
    static const std::pair<const char*, SoundSearch::Scope> scopes[] = {
        {"Project", SoundSearch::Scope::Project}, {"Essentials", SoundSearch::Scope::Essentials}, {"Library", SoundSearch::Scope::Library}};
    for (const auto& [name, value] : scopes) {
        if (ImGui::RadioButton(name, scope == value)) {
            scope = value;
            indexStale = true;
            indexEntries.clear();
            results.clear();
            cursor = -1;
            selection.clear();
        }
        UIProbe::item((std::string("soundBrowser.source.") + name).c_str());
        ImGui::SameLine();
    }
    if (searchFocusRequested) {
        ImGui::SetKeyboardFocusHere();
        searchFocusRequested = false;
    }
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 240.0f));
    if (ImGui::InputTextWithHint("##sound_search", "Search sounds", searchText, sizeof(searchText))) resultsStale = true;
    UIProbe::item("soundBrowser.search");
    ImGui::SameLine();
    ImGui::BeginDisabled(!selectedEntry() || !selectedEntry()->analysed);
    if (ImGui::Button("Similar to Selected")) {
        similarTo = selectedEntry()->path;
        resultsStale = true;
    }
    ImGui::EndDisabled();
    if (!similarTo.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##similar")) {
            similarTo.clear();
            resultsStale = true;
        }
    }

    // the type chips and the character filters
    auto chip = [&](const char* label, std::string& value, const char* wanted) {
        const bool on = value == wanted;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::SmallButton(label)) {
            value = on ? std::string() : std::string(wanted);
            resultsStale = true;
        }
        if (on) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##sound_type", typeFilter.empty() ? "Any type" : typeFilter.c_str())) {
        if (ImGui::Selectable("Any type", typeFilter.empty())) {
            typeFilter.clear();
            resultsStale = true;
        }
        for (const SoundTypes::SoundType& type : SoundTypes::all()) {
            if (ImGui::Selectable(type.name, typeFilter == type.name)) {
                typeFilter = type.name;
                resultsStale = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    chip("Dark", filters.brightness, "dark");
    chip("Neutral", filters.brightness, "neutral");
    chip("Bright", filters.brightness, "bright");
    chip("Sharp", filters.attack, "sharp");
    chip("Soft", filters.attack, "soft");
    chip("Clip", filters.kind, "clip");
    chip("Event", filters.kind, "event");
    auto tri = [&](const char* label, int& value) {
        const bool on = value == 1;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::SmallButton(label)) {
            value = on ? -1 : 1;
            resultsStale = true;
        }
        if (on) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    tri("Tonal", filters.tonal);
    tri("Loop", filters.loop);
    ImGui::SetNextItemWidth(70.0f);
    float maxSeconds = filters.maxMs < 0.0 ? 0.0f : (float)(filters.maxMs / 1000.0);
    if (ImGui::DragFloat("##max_seconds", &maxSeconds, 0.05f, 0.0f, 600.0f, maxSeconds <= 0.0f ? "Any length" : "< %.1f s")) {
        filters.maxMs = maxSeconds <= 0.0f ? -1.0 : maxSeconds * 1000.0;
        resultsStale = true;
    }
    // a row of their own: the filters fill the default window's width
    if (previewHandle != 0) {
        if (ImGui::SmallButton(ICON_FA_STOP "##preview_stop")) stopPreview();
        UIProbe::item("soundBrowser.previewPlaying");
        ImGui::SameLine();
    }
    bool autoplay = AppSettings::getSoundBrowserAutoplay();
    if (ImGui::Checkbox("Auto-play", &autoplay)) {
        AppSettings::setSoundBrowserAutoplay(autoplay);
        AppSettings::saveSettings();
    }
    ImGui::SameLine();
    bool matched = AppSettings::getSoundBrowserMatchLoudness();
    if (ImGui::Checkbox("Match loudness", &matched)) {
        AppSettings::setSoundBrowserMatchLoudness(matched);
        AppSettings::saveSettings();
    }
}

void SoundBrowserWindow::drawMiniWave(const SoundSearch::Entry& entry, float x, float y, float width, float height) {
    std::shared_ptr<const std::vector<float>> levels;
    if (entry.source == "project" && entry.kind == "clip") {
        const AudioWaveform::Result wave = AudioWaveform::get(project, entry.file);
        if (wave.data) levels = waveLevels(*wave.data);
    } else if (entry.source != "project") {
        auto found = builtinWaves.find(entry.path);
        if (found != builtinWaves.end()) {
            levels = found->second;
        } else if (!waveJob) {
            auto job = std::make_shared<WaveJob>();
            job->reference = entry.path;
            job->cacheDirectory = cacheDirectory;
            waveJob = job;
            std::thread([job]() {
                std::vector<BuiltinSoundImport::Clip> clips;
                std::string error, bytes;
                job->levels = std::make_shared<std::vector<float>>();
                if (BuiltinSoundImport::resolve({job->reference}, clips, error) && clips.front().read(bytes, error)) {
                    AudioWaveform::Data data;
                    if (AudioWaveform::ofBytes(bytes, job->cacheDirectory, data, error)) job->levels = waveLevels(data);
                }
                job->done = true;
            }).detach();
        }
    }
    if (!levels || levels->empty()) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 colour = ImGui::GetColorU32(ImGuiCol_PlotLines);
    const float mid = y + height * 0.5f;
    for (int i = 0; i < (int)levels->size(); i++) {
        const float at = x + width * (float)i / (float)levels->size();
        const float half = std::max(0.5f, (*levels)[i] * height * 0.5f);
        draw->AddLine(ImVec2(at, mid - half), ImVec2(at, mid + half), colour);
    }
}

void SoundBrowserWindow::beginCardDrag(int index) {
    if (!ImGui::BeginDragDropSource()) return;
    if (selection.count(index) == 0) select(index, false);
    const bool builtin = results[index].source != "project";
    std::vector<char> buffer;
    int count = 0;
    for (int i : selection) {
        if ((results[i].source != "project") != builtin) continue;
        const std::string text = builtin ? builtinReference(results[i]) : FileUtils::pathToGenericUtf8(results[i].file);
        buffer.insert(buffer.end(), text.begin(), text.end());
        buffer.push_back('\0');
        count++;
    }
    ImGui::SetDragDropPayload(builtin ? BUILTIN_PAYLOAD : "resource_files", buffer.data(), buffer.size());
    if (count == 1) ImGui::Text("%s", results[index].name.c_str());
    else ImGui::Text("%d sounds", count);
    ImGui::EndDragDropSource();
}

void SoundBrowserWindow::drawCard(int index, float width, float height) {
    const SoundSearch::Entry& entry = results[index];
    ImGui::PushID(index);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool selected = selection.count(index) > 0;
    if (ImGui::Selectable("##card", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(width, height))) {
        select(index, ImGui::GetIO().KeyCtrl);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) useSelected();
    }
    // the card, not the Selectable's item rect: that one reaches half a spacing past it, out of the child's clip
    UIProbe::rect("soundBrowser.card." + entry.name, origin.x, origin.y, origin.x + width, origin.y + height);
    beginCardDrag(index);
    if (scrollToCursor && index == cursor) {
        ImGui::SetScrollHereY();
        scrollToCursor = false;
    }
    const float pad = 6.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), ImGui::GetColorU32(ImGuiCol_Border));
    drawMiniWave(entry, origin.x + pad, origin.y + pad + ImGui::GetFontSize() + 2.0f, width - 2.0f * pad, height - 2.0f * ImGui::GetFontSize() - 2.0f * pad - 4.0f);

    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 faint = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    std::string title = (entry.kind == "event" ? std::string(ICON_FA_LAYER_GROUP " ") : std::string()) + entry.name;
    ImGui::PushClipRect(origin, ImVec2(origin.x + width - pad, origin.y + height), true);
    draw->AddText(ImVec2(origin.x + pad, origin.y + pad), previewPath == entry.path ? ImGui::GetColorU32(ImGuiCol_PlotHistogram) : text, title.c_str());
    char facts[96];
    std::snprintf(facts, sizeof(facts), "%.2f s  %s", entry.durationMs / 1000.0, std::isfinite(entry.lufs) ? (std::to_string((int)std::lround(entry.lufs)) + " LUFS").c_str() : "silent");
    const float bottom = origin.y + height - pad - ImGui::GetFontSize();
    draw->AddText(ImVec2(origin.x + pad, bottom), faint, facts);
    std::string badges = entry.type;
    if (!entry.licence.is_null() && entry.licence.contains("id")) badges += (badges.empty() ? "" : "  ") + entry.licence["id"].get<std::string>();
    if (!badges.empty()) {
        const float size = ImGui::CalcTextSize(badges.c_str()).x;
        draw->AddText(ImVec2(origin.x + width - pad - size, bottom), faint, badges.c_str());
    }
    ImGui::PopClipRect();
    ImGui::PopID();
}

void SoundBrowserWindow::drawCards() {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, (int)((available + spacing) / (CARD_WIDTH + spacing)));
    const int rows = ((int)results.size() + columns - 1) / columns;
    if (scrollToCursor && cursor >= 0) {
        // the row the clipper may not draw
        ImGui::SetScrollY((float)(cursor / columns) * (CARD_HEIGHT + ImGui::GetStyle().ItemSpacing.y));
        scrollToCursor = false;
    }
    ImGuiListClipper clipper;
    clipper.Begin(rows, CARD_HEIGHT + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            for (int column = 0; column < columns; column++) {
                const int index = row * columns + column;
                if (index >= (int)results.size()) break;
                if (column > 0) ImGui::SameLine();
                drawCard(index, CARD_WIDTH, CARD_HEIGHT);
            }
        }
    }
    clipper.End();
}

void SoundBrowserWindow::show() {
    updatePreview();
    if (!windowOpen) {
        focused = false;
        return;
    }
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);
    // clear of the Hierarchy and the Inspector, the windows a card is dragged to
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.2f, viewport->WorkPos.y + 60.0f), ImGuiCond_FirstUseEver);
    // the window's keys are commands: ImGui's own Space on the clicked card would also select that card
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen, ImGuiWindowFlags_NoNavInputs)) {
        focused = false;
        ImGui::End();
        return;
    }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    UIProbe::item("soundBrowser");

    if (indexStale) startIndex();
    if (indexJob && indexJob->done) {
        indexEntries = std::move(indexJob->entries);
        indexJob.reset();
        resultsStale = true;
    }
    if (waveJob && waveJob->done) {
        builtinWaves[waveJob->reference] = waveJob->levels;
        waveJob.reset();
    }

    drawToolbar();
    if (resultsStale && !indexJob) runSearch();
    ImGui::Separator();

    if (scope == SoundSearch::Scope::Library && !SoundLibrary::get().status().available) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("The starter library is not installed next to this editor. It comes with the editor's full download; "
                            "%s points the editor at a copy elsewhere.", SoundLibrary::ENV_DIR);
        ImGui::PopTextWrapPos();
    } else if (indexJob) {
        ImGui::TextDisabled("Reading sounds...");
    } else if (!searchError.empty()) {
        ImGui::TextColored(Theme::Colors::WarningText, "%s", searchError.c_str());
    } else if (results.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", hint.text.empty() ? "No sounds here." : hint.text.c_str());
        ImGui::PopTextWrapPos();
        for (const std::string& suggestion : hint.queries) {
            if (ImGui::SmallButton(suggestion.c_str())) {
                std::snprintf(searchText, sizeof(searchText), "%s", suggestion.c_str());
                resultsStale = true;
            }
            ImGui::SameLine();
        }
    } else {
        ImGui::TextDisabled("%zu sound(s)", totalResults);
        // a project sound's detail is the Inspector's, which follows the selection; a built-in clip has no file yet
        const SoundSearch::Entry* shown = selectedEntry();
        const bool detail = shown && shown->source != "project";
        ImGui::BeginChild("##sound_cards", ImVec2(0.0f, detail ? -2.2f * ImGui::GetTextLineHeightWithSpacing() : 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
        drawCards();
        ImGui::EndChild();
        if (detail) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(shown->description.empty() ? shown->name.c_str() : shown->description.c_str());
            if (!shown->licence.is_null()) ImGui::TextDisabled("Licence: %s  Enter imports it", shown->licence.value("id", "").c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::End();
}
