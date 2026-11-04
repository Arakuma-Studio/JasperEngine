// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector of Sound Events (docs/specs/S37-sound-events.md R15, R16): a .soundevent's editor,
// where every edit is a named undo step of its file, Match Levels and Audition x8 off the main thread,
// and the AudioSource rows that play one.

#include "InspectorWindow.h"

#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "audio/AudioMixer.h"
#include "audio/SoundEventDocument.h"
#include "audio/SoundEventRender.h"
#include "audio/SoundSearch.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/AudioSourceComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/UIComponent.h"
#include "external/IconsFontAwesome6.h"
#include "organisation/AssetLabels.h"
#include "subsystem/AudioSystem.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include "imgui.h"
#include "soloud.h"
#include "soloud_wav.h"

#include <atomic>
#include <memory>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct SoundEventWork {
        SoundEventDef def;
        std::string key;
        SoundEventRender::Input input;
        uint64_t seed = 1;
        std::string error;
        std::vector<SoundEventRender::Rendition> renditions;
        std::atomic<bool> done{false};
    };

    // what the view of the inspected event holds between frames
    struct SoundEventView {
        fs::path asset;
        std::shared_ptr<SoundEventWork> audition;
        std::shared_ptr<SoundEventWork> matching;
        // the renditions as preview voices, played one after another: never in the game's mix
        std::vector<std::shared_ptr<SoLoud::Wav>> samples;
        int playing = -1;
        unsigned int handle = 0;
        std::vector<std::string> analysis;
        std::shared_ptr<std::atomic<bool>> analyzing;
        std::shared_ptr<std::vector<std::string>> analyzed;
        std::string analyzedText;
        bool wantAudition = false;
        bool wantMatch = false;
        // one undo step per drag of one value
        unsigned int dragSerial = 0;
    };

    SoundEventView& soundEventView() {
        static auto* view = new SoundEventView();
        return *view;
    }

    void stopAuditionVoice(SoundEventView& view) {
        if (view.handle != 0 && AudioSystem::isInited()) AudioSystem::getSoloud().stop(view.handle);
        view.handle = 0;
        view.playing = -1;
    }

    void playAuditionVoice(SoundEventView& view, int index) {
        stopAuditionVoice(view);
        if (index < 0 || index >= (int)view.samples.size() || !AudioSystem::init()) return;
        view.handle = AudioSystem::getSoloud().play(*view.samples[index]);
        view.playing = index;
    }

    // the voices follow one another while the view is drawn
    void advanceAudition(SoundEventView& view) {
        if (view.playing < 0 || !AudioSystem::isInited() || AudioSystem::getSoloud().isValidVoiceHandle(view.handle)) return;
        const int next = view.playing + 1;
        view.handle = 0;
        view.playing = -1;
        if (next < (int)view.samples.size()) playAuditionVoice(view, next);
    }

    std::string eventKey(Project* project, const fs::path& asset) {
        const std::string guid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(asset)).str();
        return guid.empty() ? project->normalizeToAssetsRelative(asset).generic_string() : guid;
    }

}

void editor::InspectorWindow::requestSoundEventAudition() {
    soundEventView().wantAudition = true;
}

void editor::InspectorWindow::requestSoundEventMatchLevels() {
    soundEventView().wantMatch = true;
}

void editor::InspectorWindow::drawSoundEventAsset(const fs::path& asset) {
    SoundEventView& view = soundEventView();
    if (view.asset != asset) {
        stopAuditionVoice(view);
        view.samples.clear();
        view.audition.reset();
        view.matching.reset();
        view.analysis.clear();
        view.analyzedText.clear();
        view.asset = asset;
    }

    drawAssetHeader(ICON_FA_MUSIC, asset, "Sound Event");
    ImGui::Separator();

    SoundEventDef def;
    std::string error;
    if (!SoundEventDocument::read(asset, def, &error)) {
        ImGui::TextColored(Theme::Colors::WarningText, "Cannot read this Sound Event: %s", error.c_str());
        return;
    }

    auto save = [&](const SoundEventDef& changed, const char* name, const char* dragKey = nullptr) {
        std::string writeError;
        const std::string mergeKey = dragKey ? std::string(dragKey) + "#" + std::to_string(view.dragSerial) : std::string();
        if (!SoundEventDocument::commit(project, asset, changed, name, mergeKey, &writeError)) {
            Out::error("Sound Event: " + writeError);
        }
    };
    auto dragged = [&]() {
        if (ImGui::IsItemDeactivated()) view.dragSerial++;
    };

    float labelSize = getLabelSize("Cooldown (ms)");
    if (ImGui::BeginTable("sound_event_values", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelSize);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](const char* label) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
        };

        row("Mode");
        static const char* modes[] = {"Random", "Shuffle", "Sequence", "Layer"};
        int mode = (int)def.mode;
        if (ImGui::Combo("##mode", &mode, modes, 4)) {
            SoundEventDef changed = def;
            changed.mode = (SoundEventMode)mode;
            save(changed, "Set Sound Event Mode");
        }
        if (def.mode == SoundEventMode::Random) {
            row("Avoid Repeat");
            bool avoid = def.avoidRepeat;
            if (ImGui::Checkbox("##avoid", &avoid)) {
                SoundEventDef changed = def;
                changed.avoidRepeat = avoid;
                save(changed, "Set Sound Event Avoid Repeat");
            }
        }

        auto range = [&](const char* label, const char* id, float min, float max, float low, float high, const char* format, float SoundEventDef::*fieldMin,
                         float SoundEventDef::*fieldMax, const char* undo) {
            row(label);
            float a = min, b = max;
            if (ImGui::DragFloatRange2(id, &a, &b, 0.05f, low, high, format, format, ImGuiSliderFlags_AlwaysClamp)) {
                SoundEventDef changed = def;
                changed.*fieldMin = a;
                changed.*fieldMax = b;
                save(changed, undo, id);
            }
            dragged();
        };
        range("Pitch (st)", "##pitch", def.pitchMin, def.pitchMax, -24.0f, 24.0f, "%.2f", &SoundEventDef::pitchMin, &SoundEventDef::pitchMax, "Set Sound Event Pitch");
        range("Volume (dB)", "##volume", def.volumeMin, def.volumeMax, -60.0f, 12.0f, "%.1f", &SoundEventDef::volumeMin, &SoundEventDef::volumeMax, "Set Sound Event Volume");
        range("Delay (ms)", "##delay", def.delayMin, def.delayMax, 0.0f, 2000.0f, "%.0f", &SoundEventDef::delayMin, &SoundEventDef::delayMax, "Set Sound Event Delay");

        row("Output");
        const std::string shown = def.outputGroupName.empty() ? "(the source's)" : def.outputGroupName;
        if (ImGui::BeginCombo("##output", shown.c_str())) {
            if (ImGui::Selectable("(the source's)", def.outputGroupName.empty())) {
                SoundEventDef changed = def;
                changed.outputGroupId.clear();
                changed.outputGroupName.clear();
                save(changed, "Set Sound Event Output");
            }
            for (const std::string& group : AudioMixer::getGroupNames()) {
                if (ImGui::Selectable(group.c_str(), group == def.outputGroupName)) {
                    SoundEventDef changed = def;
                    changed.outputGroupName = group;
                    changed.outputGroupId = AudioMixer::findGroupId(group);
                    save(changed, "Set Sound Event Output");
                }
            }
            ImGui::EndCombo();
        }

        row("Max Instances");
        int instances = def.maxInstances;
        if (ImGui::DragInt("##instances", &instances, 0.2f, 0, 255, instances == 0 ? "No limit" : "%d", ImGuiSliderFlags_AlwaysClamp)) {
            SoundEventDef changed = def;
            changed.maxInstances = instances;
            save(changed, "Set Sound Event Max Instances", "instances");
        }
        dragged();
        row("Steal");
        static const char* steals[] = {"Oldest", "Quietest", "Newest", "None"};
        int steal = (int)def.steal;
        if (ImGui::Combo("##steal", &steal, steals, 4)) {
            SoundEventDef changed = def;
            changed.steal = (AudioStealPolicy)steal;
            save(changed, "Set Sound Event Steal");
        }
        row("Priority");
        int priority = def.priority;
        if (ImGui::DragInt("##priority", &priority, 0.5f, 0, 256, priority == 128 ? "128 (the source's)" : "%d", ImGuiSliderFlags_AlwaysClamp)) {
            SoundEventDef changed = def;
            changed.priority = priority;
            save(changed, "Set Sound Event Priority", "priority");
        }
        dragged();
        row("Cooldown (ms)");
        float cooldown = def.cooldownMs;
        if (ImGui::DragFloat("##cooldown", &cooldown, 1.0f, 0.0f, 60000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) {
            SoundEventDef changed = def;
            changed.cooldownMs = cooldown;
            save(changed, "Set Sound Event Cooldown", "cooldown");
        }
        dragged();
        row("Loop");
        bool loop = def.loop;
        if (ImGui::Checkbox("##loop", &loop)) {
            SoundEventDef changed = def;
            changed.loop = loop;
            save(changed, "Set Sound Event Loop");
        }
        row("Type");
        if (ImGui::BeginCombo("##type", def.type.empty() ? "(none)" : def.type.c_str())) {
            if (ImGui::Selectable("(none)", def.type.empty())) {
                SoundEventDef changed = def;
                changed.type.clear();
                save(changed, "Set Sound Event Type");
            }
            for (const std::string& type : SoundEventIO::soundTypes()) {
                if (ImGui::Selectable(type.c_str(), type == def.type)) {
                    SoundEventDef changed = def;
                    changed.type = type;
                    save(changed, "Set Sound Event Type");
                }
            }
            ImGui::EndCombo();
        }
        row("Labels");
        std::vector<std::string> labels;
        AssetLabels::readFileLabels(asset, false, labels);
        ImGui::TextDisabled("%s", labels.empty() ? "(none: set them in the Project window's bar)" : AssetLabels::formatList(labels).c_str());
        ImGui::EndTable();
    }

    // the 3D override
    bool spatial = def.spatial.has_value();
    if (ImGui::Checkbox("Override 3D Settings", &spatial)) {
        SoundEventDef changed = def;
        if (spatial) changed.spatial = SoundEventSpatial();
        else changed.spatial.reset();
        save(changed, "Set Sound Event 3D Settings");
    }
    if (def.spatial && ImGui::BeginTable("sound_event_spatial", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, labelSize);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        auto field = [&](const char* label, const char* id, float SoundEventSpatial::*member, float low, float high) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            float value = (*def.spatial).*member;
            if (ImGui::DragFloat(id, &value, 0.05f, low, high, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                SoundEventDef changed = def;
                (*changed.spatial).*member = value;
                save(changed, "Set Sound Event 3D Settings", id);
            }
            dragged();
        };
        field("Min Distance", "##min", &SoundEventSpatial::minDistance, 0.001f, 100000.0f);
        field("Max Distance", "##max", &SoundEventSpatial::maxDistance, 0.001f, 100000.0f);
        field("Rolloff", "##rolloff", &SoundEventSpatial::rolloff, 0.0f, 100.0f);
        field("Doppler", "##doppler", &SoundEventSpatial::doppler, 0.0f, 10.0f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Attenuation");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        static const char* attenuations[] = {"None", "Inverse Distance", "Linear Distance", "Exponential Distance"};
        int attenuation = (int)def.spatial->attenuation;
        if (ImGui::Combo("##attenuation", &attenuation, attenuations, 4)) {
            SoundEventDef changed = def;
            changed.spatial->attenuation = (AudioSourceAttenuation)attenuation;
            save(changed, "Set Sound Event 3D Settings");
        }
        ImGui::EndTable();
    }

    separatorText("Clips");
    int remove = -1, up = -1;
    for (int i = 0; i < (int)def.clips.size(); i++) {
        const SoundEventClip& clip = def.clips[i];
        ImGui::PushID(i);
        if (ImGui::SmallButton(ICON_FA_XMARK)) remove = i;
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton(ICON_FA_ARROW_UP)) up = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(clip.clip.path.c_str());
        const float width = ImGui::GetContentRegionAvail().x / 4.0f - ImGui::GetStyle().ItemSpacing.x;
        auto value = [&](const char* id, const char* format, float SoundEventClip::*member, float low, float high, const char* undo, bool same) {
            if (same) ImGui::SameLine();
            ImGui::SetNextItemWidth(width);
            float current = clip.*member;
            if (ImGui::DragFloat(id, &current, 0.05f, low, high, format, ImGuiSliderFlags_AlwaysClamp)) {
                SoundEventDef changed = def;
                changed.clips[i].*member = current;
                save(changed, undo, (std::string(id) + std::to_string(i)).c_str());
            }
            dragged();
        };
        ImGui::BeginDisabled(def.mode != SoundEventMode::Random);
        value("##weight", "weight %.2f", &SoundEventClip::weight, 0.0f, 1000.0f, "Set Sound Event Clip Weight", false);
        ImGui::EndDisabled();
        value("##db", "%.1f dB", &SoundEventClip::volumeDb, -60.0f, 12.0f, "Set Sound Event Clip Volume", true);
        value("##st", "%.2f st", &SoundEventClip::pitchSemitones, -24.0f, 24.0f, "Set Sound Event Clip Pitch", true);
        value("##ms", "%.0f ms", &SoundEventClip::delayMs, 0.0f, 2000.0f, "Set Sound Event Clip Delay", true);
        ImGui::PopID();
    }
    if (remove >= 0) {
        SoundEventDef changed = def;
        changed.clips.erase(changed.clips.begin() + remove);
        save(changed, "Remove Sound Event Clip");
    } else if (up > 0) {
        SoundEventDef changed = def;
        std::swap(changed.clips[up], changed.clips[up - 1]);
        save(changed, "Reorder Sound Event Clips");
    }
    ImGui::Button("Drop audio clips here", ImVec2(-1, 0));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            SoundEventDef changed = def;
            for (const std::string& path : Util::getStringsFromPayload(payload)) {
                if (!Util::isAudioFile(path) || !project->isInsideAssetsPath(path) || changed.clips.size() >= 64) continue;
                SoundEventClip clip;
                clip.clip = SoundEventDocument::clipRef(project, path, nullptr);
                changed.clips.push_back(std::move(clip));
            }
            save(changed, "Add Sound Event Clips");
        }
        ImGui::EndDragDropTarget();
    }

    // the problems: at once what the file says, and what decoding its clips says once it finishes
    const std::string text = SoundEventIO::write(def);
    if (text != view.analyzedText && (!view.analyzing || view.analyzing->load())) {
        view.analyzedText = text;
        if (view.analyzed && view.analyzing) view.analysis = *view.analyzed;
        auto finished = std::make_shared<std::atomic<bool>>(false);
        auto found = std::make_shared<std::vector<std::string>>();
        view.analyzing = finished;
        view.analyzed = found;
        const SoundEventRender::Input input = SoundEventRender::prepare(*project, def);
        std::thread([def, input, finished, found]() {
            int analysed = 0;
            *found = SoundSearch::eventProblems(def, input.clipFiles, input.cacheDirectory, analysed);
            finished->store(true);
        }).detach();
    }
    if (view.analyzing && view.analyzing->load()) view.analysis = *view.analyzed;
    std::vector<std::string> problems = SoundEventDocument::problems(project, def);
    problems.insert(problems.end(), view.analysis.begin(), view.analysis.end());
    if (!problems.empty()) {
        separatorText("Problems");
        for (const std::string& problem : problems) ImGui::TextColored(Theme::Colors::WarningText, "%s", problem.c_str());
    }

    separatorText("Audition");
    const bool layer = def.mode == SoundEventMode::Layer;
    ImGui::BeginDisabled(layer || def.clips.empty() || view.matching != nullptr);
    if (ImGui::Button("Match Levels")) view.wantMatch = true;
    UIProbe::item("soundEvent.matchLevels");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(def.clips.empty() || view.audition != nullptr);
    if (ImGui::Button("Audition x8")) view.wantAudition = true;
    UIProbe::item("soundEvent.audition");
    ImGui::EndDisabled();
    if (view.playing >= 0) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP "##audition_stop")) stopAuditionVoice(view);
    }

    if (view.wantMatch) {
        view.wantMatch = false;
        if (!layer && !def.clips.empty() && !view.matching) {
            view.matching = std::make_shared<SoundEventWork>();
            view.matching->def = def;
            view.matching->input = SoundEventRender::prepare(*project, def);
            std::shared_ptr<SoundEventWork> work = view.matching;
            std::thread([work]() {
                SoundEventRender::matchLevels(work->def, work->input, work->error);
                work->done = true;
            }).detach();
        }
    }
    if (view.matching && view.matching->done) {
        if (view.matching->error.empty()) save(view.matching->def, "Match Sound Event Levels");
        else Out::warning("Match Levels: %s", view.matching->error.c_str());
        view.matching.reset();
    }

    if (view.wantAudition) {
        view.wantAudition = false;
        // previews are held while the game plays: its voices stay what a capture hears
        if (!def.clips.empty() && !view.audition) {
            stopAuditionVoice(view);
            view.samples.clear();
            view.audition = std::make_shared<SoundEventWork>();
            view.audition->def = def;
            view.audition->key = eventKey(project, asset);
            view.audition->input = SoundEventRender::prepare(*project, def);
            view.audition->seed = (uint64_t)project->getAudioSettings().seed;
            std::shared_ptr<SoundEventWork> work = view.audition;
            std::thread([work]() {
                SoundEventRender::render(work->def, work->key, work->input, 8, work->seed, work->renditions, work->error);
                work->done = true;
            }).detach();
        }
    }
    if (view.audition && view.audition->done && view.samples.empty()) {
        for (const SoundEventRender::Rendition& rendition : view.audition->renditions) {
            auto wav = std::make_shared<SoLoud::Wav>();
            const unsigned int count = (unsigned int)rendition.samples.size();
            // planar copies: SoLoud's raw float wave is one buffer per channel
            std::vector<float> planar(count);
            const unsigned int frames = count / SoundEventRender::CHANNELS;
            for (unsigned int f = 0; f < frames; f++) {
                planar[f] = rendition.samples[f * 2];
                planar[frames + f] = rendition.samples[f * 2 + 1];
            }
            if (count > 0) wav->loadRawWave(planar.data(), count, (float)SoundEventRender::SAMPLE_RATE, SoundEventRender::CHANNELS, true, false);
            view.samples.push_back(wav);
        }
        if (!view.samples.empty() && !project->isAnyScenePlaying()) playAuditionVoice(view, 0);
    }
    advanceAudition(view);
    if (view.audition && view.audition->done) {
        if (!view.audition->error.empty()) ImGui::TextColored(Theme::Colors::WarningText, "%s", view.audition->error.c_str());
        const std::vector<SoundEventRender::Rendition>& renditions = view.audition->renditions;
        for (int i = 0; i < (int)renditions.size(); i++) {
            std::string clips;
            for (const SoundEventRender::Voice& voice : renditions[i].voices) clips += (clips.empty() ? "" : ", ") + fs::path(voice.clip).filename().string();
            const std::string line = std::to_string(i + 1) + ". " + clips;
            if (ImGui::Selectable((line + "##rendition" + std::to_string(i)).c_str(), view.playing == i)) playAuditionVoice(view, i);
            UIProbe::item("soundEvent.rendition." + std::to_string(i));
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f LUFS", renditions[i].lufs);
        }
    }
}

void editor::InspectorWindow::drawAudioEventRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities) {
    Scene* scene = sceneProject->scene;
    const AudioSourceComponent& first = scene->getComponent<AudioSourceComponent>(entities.front());

    beginTable(cpType, getLabelSize("Full Volume Speed"), "audio_event_rows");
    propertyRow(RowPropertyType::Asset, cpType, "event", "Event", sceneProject, entities);

    // Play On, as checkbox chips
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Play On");
    ImGui::TableNextColumn();
    struct Chip { uint32_t flag; const char* label; };
    static constexpr Chip chips[] = {{AudioPlayOn_UIClick, "UI Click"}, {AudioPlayOn_UIPress, "UI Press"}, {AudioPlayOn_UIHover, "UI Hover"},
                                     {AudioPlayOn_Collision2D, "Collision 2D"}, {AudioPlayOn_Collision3D, "Collision 3D"},
                                     {AudioPlayOn_Sensor2D, "Sensor 2D"}, {AudioPlayOn_Sensor3D, "Sensor 3D"}};
    const float right = ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX();
    for (const Chip& chip : chips) {
        bool on = (first.playOn & chip.flag) != 0;
        if (ImGui::Checkbox(chip.label, &on)) {
            auto* multi = new MultiPropertyCmd();
            for (Entity entity : entities) {
                const uint32_t flags = scene->getComponent<AudioSourceComponent>(entity).playOn;
                multi->addPropertyCmd<unsigned int>(project, sceneProject->id, entity, cpType, "playOn", on ? (flags | chip.flag) : (flags & ~chip.flag));
            }
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(multi);
        }
        const float next = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("Collision 2D").x + 2.0f * ImGui::GetFontSize();
        if (&chip != &chips[6] && next < right + ImGui::GetWindowPos().x) ImGui::SameLine();
    }

    const bool collisions = first.playOn & (AudioPlayOn_Collision2D | AudioPlayOn_Collision3D);
    if (collisions) {
        RowSettings speed;
        speed.stepSize = 0.1f;
        speed.format = "%.2f";
        speed.help = "A contact slower than this plays nothing";
        propertyRow(RowPropertyType::FloatPositive, cpType, "minImpactSpeed", "Min Impact Speed", sceneProject, entities, speed);
        speed.help = "The speed at full volume; a slower contact is quieter. 0 = no scaling";
        propertyRow(RowPropertyType::FloatPositive, cpType, "fullVolumeImpactSpeed", "Full Volume Speed", sceneProject, entities, speed);
    }
    endTable();

    if (first.event.empty()) {
        separatorText("Variation");
        beginTable(cpType, getLabelSize("Random Volume (dB)"), "audio_variation_rows");
        RowSettings random;
        random.stepSize = 0.1f;
        random.format = "%.2f";
        random.help = "Each start: +- this many semitones";
        propertyRow(RowPropertyType::FloatPositive, cpType, "pitchRandomSemitones", "Random Pitch (st)", sceneProject, entities, random);
        random.help = "Each start: -this .. 0 dB, never louder";
        propertyRow(RowPropertyType::FloatPositive, cpType, "volumeRandomDb", "Random Volume (dB)", sceneProject, entities, random);
        endTable();
    }

    // what will not work as set up
    if (entities.size() == 1) {
        const Entity entity = entities.front();
        auto warn = [](const std::string& text) { ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str()); };
        if (!first.event.empty()) {
            if (!first.filename.empty()) warn("Both an Event and an Audio Clip are set: the Event plays.");
            SoundEventDef def;
            const fs::path file = project->resolveAssetPath(FileUtils::pathFromUtf8(first.event.path));
            std::string error;
            if (!SoundEventDocument::read(file, def, &error)) {
                warn("The Event does not read: " + error);
            } else {
                for (const std::string& problem : SoundEventDocument::problems(project, def)) {
                    if (problem.find("missing clip") != std::string::npos) warn("The Event " + problem);
                }
            }
        }
        if (collisions && !scene->findComponent<RigidbodyComponent>(entity) && !scene->findComponent<Rigidbody2DComponent>(entity)) {
            warn("A collision trigger needs a Rigidbody or Rigidbody 2D on this GameObject.");
        }
        if ((first.playOn & (AudioPlayOn_UIClick | AudioPlayOn_UIPress | AudioPlayOn_UIHover)) && !scene->findComponent<UIComponent>(entity)) {
            warn("A UI trigger needs a UI component on this GameObject.");
        }
    }
}
