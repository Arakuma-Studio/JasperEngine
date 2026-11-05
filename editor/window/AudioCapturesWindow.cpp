// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioCapturesWindow.h"

#include "Backend.h"
#include "Project.h"
#include "Theme.h"
#include "audio/AudioCapture.h"
#include "audio/AudioSessionReader.h"
#include "audio/AudioTimelineImage.h"
#include "jobs/JobSystem.h"
#include "render/TextureRender.h"
#include "subsystem/AudioSystem.h"
#include "texture/TextureData.h"
#include "util/UIProbe.h"

#include "imgui.h"
#include "soloud.h"
#include "soloud_wav.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    using Json = nlohmann::json;

    constexpr double REFRESH_SECONDS = 2.0;
    constexpr double MIN_VIEW_SECONDS = 0.05;
    constexpr double MOMENT_BEFORE = 0.5;
    constexpr double MOMENT_AFTER = 1.5;

    Json readCheck(const AudioAnalysis::Capture& capture) {
        std::ifstream in(capture.directory / "check.json", std::ios::binary);
        if (!in) return nullptr;
        Json check = Json::parse(in, nullptr, false);
        return check.is_discarded() ? Json(nullptr) : check;
    }

}

struct AudioCapturesWindow::RenderJob {
    std::atomic<bool> done{false};
    double start = 0.0;
    double end = 0.0;
    int width = 0;
    AudioTimelineImage::Result result;
};

struct AudioCapturesWindow::AudioJob {
    std::atomic<bool> done{false};
    std::string session;
    std::shared_ptr<std::string> bytes;
};

AudioCapturesWindow::AudioCapturesWindow(Project* project): project(project) {}

void AudioCapturesWindow::open() {
    windowOpen = true;
    refreshedAt = -1000.0;
    ImGui::SetWindowFocus(WINDOW_NAME);
}

bool AudioCapturesWindow::isOpen() const {
    return windowOpen;
}

bool AudioCapturesWindow::isFocused() const {
    return windowOpen && focused;
}

void AudioCapturesWindow::resetForProjectSwitch() {
    stopMoment();
    wav.reset();
    wavBytes.reset();
    loadedSession.clear();
    sessions.clear();
    verdicts.clear();
    selected = -1;
    check = nullptr;
    moment = -1;
    renderJob.reset();
    audioJob.reset();
    refreshedAt = -1000.0;
    if (!timeline.empty()) retired.push_back(timeline);
    timeline = Texture();
}

void AudioCapturesWindow::refreshSessions() {
    const std::string selectedId = selected >= 0 && selected < (int)sessions.size() ? sessions[selected].id : std::string();
    sessions = AudioAnalysis::listCaptures(AudioAnalysis::captureRoot(project->getProjectPath()), AudioCapture::sessionId());
    std::reverse(sessions.begin(), sessions.end());
    verdicts.clear();
    int found = -1;
    for (size_t i = 0; i < sessions.size(); i++) {
        const Json saved = readCheck(sessions[i]);
        verdicts.push_back(saved.is_object() ? saved.value("verdict", std::string()) : std::string());
        if (sessions[i].id == selectedId) found = (int)i;
    }
    selected = found;
    if (selected >= 0) {
        const Json saved = readCheck(sessions[selected]);
        if (saved != check) {
            check = saved;
            moment = -1;
            renderedWidth = 0;
        }
    } else if (!sessions.empty()) {
        selectSession(0);
    }
}

void AudioCapturesWindow::selectSession(int index) {
    stopMoment();
    selected = index;
    moment = -1;
    check = nullptr;
    renderedWidth = 0;
    if (index < 0 || index >= (int)sessions.size()) return;
    const AudioAnalysis::Capture& capture = sessions[index];
    check = readCheck(capture);
    sessionSeconds = std::max(capture.session.value("duration_s", 0.0), MIN_VIEW_SECONDS);
    viewStart = 0.0;
    viewEnd = sessionSeconds;
}

void AudioCapturesWindow::requestRender() {
    if (selected < 0 || selected >= (int)sessions.size() || renderWidth < AudioImages::MIN_WIDTH) return;
    const int width = std::min(renderWidth, AudioImages::MAX_WIDTH);
    if (renderedStart == viewStart && renderedEnd == viewEnd && renderedWidth == width) return;
    if (renderJob) {
        renderAgain = true;
        return;
    }
    renderAgain = false;
    renderedStart = viewStart;
    renderedEnd = viewEnd;
    renderedWidth = width;
    auto job = std::make_shared<RenderJob>();
    job->start = viewStart;
    job->end = viewEnd;
    job->width = width;
    renderJob = job;
    const AudioAnalysis::Capture capture = sessions[selected];
    const Json saved = check;
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "audio-captures-render";
    jobs::submit([job, capture, saved]() {
        AudioSessionData data;
        std::string error;
        if (AudioSessionReader::load(capture, job->start, job->end, data, error)) {
            AudioTimelineImage::Options lanes;
            AudioSessionReader::report(data, 2000, lanes.markers, lanes.events);
            if (saved.is_object()) {
                for (const Json& item : saved.value("checks", Json::array())) {
                    const std::string status = item.value("status", "");
                    if (status != "FAIL" && status != "WARN") continue;
                    for (const Json& at : item.value("at", Json::array())) {
                        (status == "FAIL" ? lanes.failures : lanes.warnings).push_back({at.get<double>(), item.value("id", "")});
                    }
                }
                int n = 0;
                for (const Json& listen : saved.value("listen", Json::array())) lanes.listen.push_back({listen.value("t", 0.0), "L" + std::to_string(++n)});
            }
            job->result = AudioTimelineImage::render(data, lanes, job->width);
        }
        job->done.store(true, std::memory_order_release);
    }, options);
}

void AudioCapturesWindow::pollJobs() {
    if (renderJob && renderJob->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<RenderJob> job = renderJob;
        renderJob.reset();
        AudioImages::Image& image = job->result.image;
        if (image.width > 0 && image.height > 0) {
            TextureData data(image.width, image.height, (unsigned int)image.rgba.size(), ColorFormat::RGBA, 4, image.rgba.data());
            Texture next;
            next.setData("editor:audio-captures:" + std::to_string(++textureGeneration), data);
            next.setMinFilter(TextureFilter::LINEAR);
            next.setMagFilter(TextureFilter::LINEAR);
            TextureRender* render = next.getRender();
            if (render && render->isCreated()) {
                next.releaseData();
                if (!timeline.empty()) retired.push_back(timeline);
                timeline = next;
                timelineWidth = image.width;
                timelineHeight = image.height;
            } else {
                next.destroy();
            }
        }
    }
    if (audioJob && audioJob->done.load(std::memory_order_acquire)) {
        const std::shared_ptr<AudioJob> job = audioJob;
        audioJob.reset();
        if (job->bytes && !job->bytes->empty() && AudioSystem::init()) {
            auto loaded = std::make_shared<SoLoud::Wav>();
            if (loaded->loadMem(reinterpret_cast<const unsigned char*>(job->bytes->data()), (unsigned int)job->bytes->size(), false, false) == SoLoud::SO_NO_ERROR) {
                wav = loaded;
                wavBytes = job->bytes;
                loadedSession = job->session;
                if (pendingMoment >= 0.0) startMoment(pendingMoment);
            }
        }
        pendingMoment = -1.0;
    }
    if (previewHandle != 0 && (!AudioSystem::isInited() || !AudioSystem::getSoloud().isValidVoiceHandle(previewHandle))) previewHandle = 0;
}

void AudioCapturesWindow::startMoment(double seconds) {
    stopMoment();
    if (!wav || !AudioSystem::init()) return;
    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    const double from = std::max(0.0, seconds - MOMENT_BEFORE);
    // outside the mixer: what was captured is heard as it was, whatever the groups say now
    previewHandle = soloud.play(*wav, 1.0f, 0.0f, true);
    if (previewHandle == 0) return;
    soloud.seek(previewHandle, from);
    soloud.scheduleStop(previewHandle, seconds + MOMENT_AFTER - from);
    soloud.setPause(previewHandle, false);
}

void AudioCapturesWindow::playMoment() {
    if (AudioCapture::isActive() || selected < 0 || selected >= (int)sessions.size() || !check.is_object()) return;
    const Json listen = check.value("listen", Json::array());
    if (listen.empty()) return;
    if (moment < 0 || moment >= (int)listen.size()) moment = 0;
    const double seconds = listen[moment].value("t", 0.0);
    const AudioAnalysis::Capture& capture = sessions[selected];
    if (loadedSession == capture.id && wav) {
        startMoment(seconds);
        return;
    }
    pendingMoment = seconds;
    if (audioJob && audioJob->session == capture.id) return;
    auto job = std::make_shared<AudioJob>();
    job->session = capture.id;
    audioJob = job;
    const fs::path file = AudioSessionReader::audioFile(capture.directory, capture.session.value("files", Json::object()).value("master", std::string("master.wav")));
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "audio-captures-load";
    jobs::submit([job, file]() {
        std::ifstream in(file, std::ios::binary);
        std::stringstream bytes;
        bytes << in.rdbuf();
        job->bytes = std::make_shared<std::string>(bytes.str());
        job->done.store(true, std::memory_order_release);
    }, options);
}

void AudioCapturesWindow::stopMoment() {
    if (previewHandle != 0 && AudioSystem::isInited()) AudioSystem::getSoloud().stop(previewHandle);
    previewHandle = 0;
    pendingMoment = -1.0;
}

void AudioCapturesWindow::selectMoment(int direction) {
    if (!check.is_object()) return;
    const int count = (int)check.value("listen", Json::array()).size();
    if (count == 0) return;
    moment = moment < 0 ? 0 : std::clamp(moment + direction, 0, count - 1);
}

void AudioCapturesWindow::drawSessions() {
    if (sessions.empty()) {
        ImGui::TextDisabled("No captures yet: a Play captures when the editor has no output device, or after audio_session arm.");
        return;
    }
    for (size_t i = 0; i < sessions.size(); i++) {
        const AudioAnalysis::Capture& capture = sessions[i];
        char line[256];
        std::snprintf(line, sizeof(line), "%s  %s  %s  %.1f s%s%s###session%zu", capture.id.c_str(), capture.state.c_str(), capture.session.value("scene", std::string()).c_str(),
                      capture.session.value("duration_s", 0.0), verdicts[i].empty() ? "" : "  ", verdicts[i].c_str(), i);
        if (ImGui::Selectable(line, (int)i == selected)) selectSession((int)i);
        UIProbe::item(("audioCaptures.session." + capture.id).c_str());
    }
}

void AudioCapturesWindow::drawTimeline() {
    renderWidth = (int)ImGui::GetContentRegionAvail().x;
    if (selected < 0) return;
    requestRender();
    if (timeline.empty()) {
        ImGui::TextDisabled("Drawing...");
        return;
    }
    TextureRender* render = timeline.getRender();
    if (!render) return;
    const ImVec2 size((float)timelineWidth, (float)timelineHeight);
    ImGui::Image(Backend::getImGuiTexture(render), size);
    UIProbe::item("audioCaptures.timeline");
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            // about the time under the pointer
            const double share = std::clamp((double)((ImGui::GetIO().MousePos.x - ImGui::GetItemRectMin().x) / std::max(1.0f, size.x)), 0.0, 1.0);
            const double pivot = viewStart + share * (viewEnd - viewStart);
            const double span = std::clamp((viewEnd - viewStart) * std::pow(0.8, (double)wheel), MIN_VIEW_SECONDS, sessionSeconds);
            viewStart = std::clamp(pivot - share * span, 0.0, sessionSeconds - span);
            viewEnd = viewStart + span;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const double span = viewEnd - viewStart;
            const double moved = -(double)ImGui::GetIO().MouseDelta.x / std::max(1.0f, size.x) * span;
            viewStart = std::clamp(viewStart + moved, 0.0, sessionSeconds - span);
            viewEnd = viewStart + span;
        }
    }
    if (renderAgain) requestRender();
}

void AudioCapturesWindow::drawChecks() {
    if (!check.is_object()) return;
    ImGui::Text("Last check: %s", check.value("verdict", std::string()).c_str());
    UIProbe::item("audioCaptures.verdict");
    for (const Json& item : check.value("checks", Json::array())) {
        const std::string status = item.value("status", "");
        const std::string limited = item["limited_by"].is_string() ? item["limited_by"].get<std::string>() : std::string();
        const std::string text = status + "  " + item.value("id", "") + (limited.empty() ? "" : ": " + limited);
        if (status == "FAIL") ImGui::TextColored(Theme::Colors::ErrorText, "%s", text.c_str());
        else if (status == "SKIP") ImGui::TextDisabled("%s", text.c_str());
        else ImGui::TextUnformatted(text.c_str());
    }
}

void AudioCapturesWindow::drawListen() {
    if (!check.is_object()) return;
    const Json listen = check.value("listen", Json::array());
    if (listen.empty()) return;
    ImGui::Separator();
    ImGui::TextUnformatted("Your agent asked you to listen to these moments");
    UIProbe::item("audioCaptures.listenBanner");
    for (size_t i = 0; i < listen.size(); i++) {
        char line[320];
        std::snprintf(line, sizeof(line), "L%zu  %.2f s  %s###moment%zu", i + 1, listen[i].value("t", 0.0), listen[i].value("why", std::string()).c_str(), i);
        if (ImGui::Selectable(line, (int)i == moment)) {
            moment = (int)i;
            playMoment();
        }
        UIProbe::item(("audioCaptures.moment." + std::to_string(i)).c_str());
    }
    if (previewHandle != 0) {
        ImGui::TextUnformatted("Playing");
        UIProbe::item("audioCaptures.previewPlaying");
    }
}

void AudioCapturesWindow::show() {
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
    if (!windowOpen) {
        focused = false;
        if (previewHandle != 0) stopMoment();
        return;
    }
    // the window's keys are commands: ImGui's own arrows and Enter would move its selection too
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen, ImGuiWindowFlags_NoNavInputs)) {
        focused = false;
        ImGui::End();
        return;
    }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    UIProbe::item("audioCaptures");

    pollJobs();
    const double now = ImGui::GetTime();
    if (now - refreshedAt > REFRESH_SECONDS) {
        refreshedAt = now;
        refreshSessions();
    }

    const bool capturing = AudioCapture::isActive();
    if (capturing) {
        stopMoment();
        ImGui::TextDisabled("A capture is running: the sessions open when Play stops.");
    }
    ImGui::BeginDisabled(capturing);
    drawSessions();
    ImGui::Separator();
    if (!capturing) drawTimeline();
    drawChecks();
    drawListen();
    ImGui::EndDisabled();
    ImGui::End();
}
