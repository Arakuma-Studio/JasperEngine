// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioSessionReader.h"

#include "io/AtomicFile.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    using Json = nlohmann::json;

    bool readJsonLines(const fs::path& file, std::vector<Json>& out) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        std::string text;
        bool first = true;
        while (std::getline(in, text)) {
            if (text.empty()) continue;
            // the last line of a session still being written may be cut
            Json line = Json::parse(text, nullptr, false);
            if (line.is_discarded() || !line.is_object()) continue;
            if (first) {
                first = false;
                if (line.contains("format")) continue;
            }
            out.push_back(std::move(line));
        }
        return true;
    }

    Json dbOrNull(const std::optional<double>& value) {
        return value ? AudioAnalysis::rounded(*value, 0.01) : Json(nullptr);
    }

    Json voiceEntry(const Json& line, int n) {
        Json out = {{"id", "E" + std::to_string(n)}, {"kind", line.value("kind", "")}, {"t", line.value("t", 0.0)}, {"clip", line.value("clip", "")}, {"group", line.value("group", "")}};
        for (const char* key : {"entity", "reason", "gain_db", "cause", "owner", "played_s", "voice"}) {
            if (line.contains(key) && !line[key].is_null()) out[key] = line[key];
        }
        return out;
    }

    Json markerEntry(const Json& line, int n) {
        Json out = {{"id", "M" + std::to_string(n)}, {"kind", line.value("kind", "")}, {"t", line.value("t", 0.0)}};
        for (const char* key : {"label", "entity", "other", "speed", "device", "action", "code", "function", "prefab", "element"}) {
            if (line.contains(key) && !line[key].is_null()) out[key] = line[key];
        }
        return out;
    }

}

bool AudioSessionData::inRange(const Json& line) const {
    const double t = seconds(line);
    return t >= startSeconds && t < endSeconds;
}

double AudioSessionData::seconds(const Json& line) const {
    return (double)line.value("sample", (uint64_t)0) / rate;
}

bool AudioSessionReader::isVoiceKind(const std::string& kind) {
    return kind.rfind("voice_", 0) == 0;
}

bool AudioSessionReader::isMarkerKind(const std::string& kind) {
    static const std::set<std::string> kinds = {"marker", "input", "collision_2d", "collision_3d", "sensor_2d", "sensor_3d", "ui_press", "ui_click",
                                                "ui_hover", "html_click", "animation_event", "spawn", "destroy", "scene_load"};
    return kinds.count(kind) > 0;
}

fs::path AudioSessionReader::audioFile(const fs::path& directory, const std::string& relative) {
    std::error_code ec;
    const fs::path published = directory / fs::path(relative);
    if (fs::exists(published, ec)) return published;
    const fs::path folder = published.parent_path();
    const std::string prefix = "." + published.filename().string() + ".";
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && io::isTemporarySiblingName(name)) return entry.path();
    }
    return {};
}

bool AudioSessionReader::load(const AudioAnalysis::Capture& capture, double start, double end, AudioSessionData& data, std::string& error) {
    data.capture = capture;
    data.rate = capture.session.value("sample_rate", 48000u);
    if (!readJsonLines(capture.directory / "events.jsonl", data.events)) {
        error = "Capture " + capture.id + " has no events.jsonl.";
        return false;
    }
    readJsonLines(capture.directory / "levels.jsonl", data.levels);

    const fs::path file = audioFile(capture.directory, capture.session.value("files", Json::object()).value("master", std::string("master.wav")));
    if (file.empty()) {
        error = "Capture " + capture.id + " has no master.wav.";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    std::stringstream bytes;
    bytes << in.rdbuf();
    AudioAnalysis::Signal whole;
    if (!in || !AudioAnalysis::readCaptureAudio(bytes.str(), whole, error)) {
        if (error.empty()) error = "Can't read " + file.filename().string() + " of capture " + capture.id + ".";
        return false;
    }
    if (whole.sampleRate > 0) data.rate = whole.sampleRate;

    double sessionSeconds = (double)AudioAnalysis::captureSamples(capture, whole) / data.rate;
    if (!data.events.empty()) sessionSeconds = std::max(sessionSeconds, data.seconds(data.events.back()));
    data.startSeconds = std::min(std::max(start, 0.0), sessionSeconds);
    data.endSeconds = end < 0.0 ? sessionSeconds : std::min(end, sessionSeconds);

    const uint64_t first = std::min<uint64_t>((uint64_t)std::llround(data.startSeconds * data.rate), whole.frames);
    const uint64_t last = std::clamp<uint64_t>((uint64_t)std::llround(data.endSeconds * data.rate), first, whole.frames);
    data.startFrame = first;
    data.audio.sampleRate = whole.sampleRate;
    data.audio.channels = whole.channels;
    data.audio.frames = (size_t)(last - first);
    data.audio.samples.resize(data.audio.frames * whole.channels);
    for (unsigned int c = 0; c < whole.channels; c++) {
        const float* from = whole.channel(c) + first;
        float* to = data.audio.samples.data() + (size_t)c * data.audio.frames;
        for (size_t i = 0; i < data.audio.frames; i++) to[i] = std::isfinite(from[i]) ? from[i] : 0.0f;
    }

    AudioMeterOptions options;
    options.mode = AudioMeterMode::Stream;
    options.bands = false;
    AudioMeter meter(data.rate, whole.channels, options);
    std::vector<const float*> planar;
    for (unsigned int c = 0; c < whole.channels; c++) planar.push_back(data.audio.channel(c));
    meter.processPlanar(planar.data(), data.audio.frames);
    data.loudness = meter.result();
    for (float value : meter.momentaryCurve()) data.momentary.push_back(value);
    for (float value : meter.shortTermCurve()) data.shortTerm.push_back(value);
    return true;
}

AudioSessionReader::Json AudioSessionReader::statusEntry(const AudioAnalysis::Capture& capture) {
    const Json& session = capture.session;
    Json entry = {{"session", capture.id}, {"scene", session.value("scene", "")}, {"state", capture.state},
                  {"started_utc", session.value("started_utc", "")}, {"duration_s", session.value("duration_s", 0.0)},
                  {"deterministic", session.value("deterministic", false)}};
    uint64_t events = 0, markers = 0;
    const Json counts = session.value("counts", Json::object());
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        if (isVoiceKind(it.key())) events += it.value().get<uint64_t>();
        else if (isMarkerKind(it.key())) markers += it.value().get<uint64_t>();
    }
    if (capture.state != "complete" && capture.state != "failed") {
        // session.json has its counts at the end: the lines say them
        std::ifstream in(capture.directory / "events.jsonl", std::ios::binary);
        std::string text;
        events = markers = 0;
        while (std::getline(in, text)) {
            const size_t at = text.find("\"kind\":\"");
            if (at == std::string::npos) continue;
            const size_t end = text.find('"', at + 8);
            if (end == std::string::npos) continue;
            const std::string kind = text.substr(at + 8, end - at - 8);
            if (isVoiceKind(kind)) events++;
            else if (isMarkerKind(kind)) markers++;
        }
    }
    entry["events"] = events;
    entry["markers"] = markers;
    return entry;
}

int AudioSessionReader::voicesPeak(const AudioSessionData& data, const std::string& group) {
    int now = 0, peak = 0;
    std::set<int64_t> counted;
    for (const Json& line : data.events) {
        const std::string kind = line.value("kind", "");
        if (kind == "voice_start" && (group.empty() || line.value("group", "") == group)) {
            counted.insert(line.value("voice", (int64_t)0));
            now++;
            if (data.inRange(line)) peak = std::max(peak, now);
        } else if (kind == "voice_stop" && counted.erase(line.value("voice", (int64_t)0)) > 0) {
            now--;
        }
    }
    return peak;
}

AudioSessionReader::Json AudioSessionReader::report(const AudioSessionData& data, int eventsLimit, std::vector<AudioImages::Label>& markerLabels,
                                                    std::vector<AudioImages::Label>& eventLabels) {
    const Json& session = data.capture.session;
    Json out;
    out["capture"] = {{"session", data.capture.id}, {"project", session.value("project", "")}, {"scene", session.value("scene", "")},
                      {"state", data.capture.state}, {"started_utc", session.value("started_utc", "")},
                      {"duration_s", session.value("duration_s", 0.0)}, {"captured_samples", session.value("captured_samples", (uint64_t)0)},
                      {"truncated", session.value("truncated", false)}, {"deterministic", session.value("deterministic", false)},
                      {"not_deterministic", session.value("not_deterministic", Json::array())},
                      {"fixed_fps", session.value("fixed_fps", 0.0)}, {"seed", session.value("seed", 0)},
                      {"latency_samples", session.value("latency_samples", 0)}, {"warnings", session.value("warnings", Json::array())}};
    if (session.contains("error") && !session["error"].is_null()) out["capture"]["error"] = session["error"];
    out["range"] = {{"start", data.startSeconds}, {"end", data.endSeconds}, {"audio_seconds", (double)data.audio.frames / data.rate}};
    out["loudness"] = {{"integrated_lufs", dbOrNull(data.loudness.integrated)}, {"short_term_max", dbOrNull(data.loudness.shortTermMax)},
                       {"momentary_max", dbOrNull(data.loudness.momentaryMax)}, {"lra", dbOrNull(data.loudness.lra)}};
    out["peak"] = {{"sample_dbfs", AudioAnalysis::rounded(data.loudness.samplePeakDb, 0.01)},
                   {"true_peak_dbtp", AudioAnalysis::rounded(data.loudness.truePeakDb, 0.01)}, {"clipped", data.loudness.clipped}};

    // the groups, from the 100 ms lines of the range
    std::map<std::string, std::pair<double, double>> levels;  // peak, summed energy
    std::map<std::string, int> levelLines;
    for (const Json& line : data.levels) {
        if (!data.inRange(line)) continue;
        const Json groups = line.value("groups", Json::object());
        for (auto it = groups.begin(); it != groups.end(); ++it) {
            auto found = levels.find(it.key());
            if (found == levels.end()) found = levels.insert({it.key(), {AudioMeter::SILENCE_DB, 0.0}}).first;
            found->second.first = std::max(found->second.first, it.value().value("peak_db", AudioMeter::SILENCE_DB));
            found->second.second += std::pow(10.0, it.value().value("rms_db", AudioMeter::SILENCE_DB) / 10.0);
            levelLines[it.key()]++;
        }
    }
    Json groups = Json::object();
    for (const auto& [name, level] : levels) {
        const double mean = level.second / std::max(1, levelLines[name]);
        groups[name] = {{"peak_db", AudioAnalysis::rounded(level.first, 0.01)},
                        {"rms_db", AudioAnalysis::rounded(mean > 0.0 ? std::max(10.0 * std::log10(mean), AudioMeter::SILENCE_DB) : AudioMeter::SILENCE_DB, 0.01)},
                        {"voices_peak", voicesPeak(data, name)}};
    }
    out["groups"] = groups;

    // numbered over the whole session, so a moment keeps its number whatever the range
    Json events = Json::array(), markers = Json::array(), mixer = Json::array(), music = Json::array();
    int eventNumber = 0, markerNumber = 0;
    int eventsTotal = 0, markersTotal = 0;
    int started = 0, stopped = 0, stolen = 0, rejected = 0;
    std::map<std::string, int> byClip;
    std::map<std::string, std::pair<std::string, int>> byEvent;   // GUID > path, starts
    for (const Json& line : data.events) {
        const std::string kind = line.value("kind", "");
        const bool voice = isVoiceKind(kind);
        const bool marker = isMarkerKind(kind);
        const int number = voice ? ++eventNumber : marker ? ++markerNumber : 0;
        if (!data.inRange(line)) continue;
        if (kind == "mixer") {
            if ((int)mixer.size() < eventsLimit) mixer.push_back(line);
        } else if (kind == "music") {
            if ((int)music.size() < eventsLimit) music.push_back(line);
        } else if (voice) {
            eventsTotal++;
            if (kind == "voice_start") {
                started++;
                byClip[line.value("clip", "")]++;
                if (line.contains("event")) {
                    auto& entry = byEvent[line.value("event", "")];
                    entry.first = line.value("event_path", "");
                    entry.second++;
                }
            } else if (kind == "voice_stop") {
                stopped++;
                if (line.value("reason", "").rfind("stolen", 0) == 0) stolen++;
            } else if (kind == "voice_reject") {
                rejected++;
            }
            if ((int)events.size() < eventsLimit) {
                events.push_back(voiceEntry(line, number));
                if (kind == "voice_start" || kind == "voice_stop") eventLabels.push_back({data.seconds(line), "E" + std::to_string(number), kind == "voice_stop"});
            }
        } else if (marker) {
            markersTotal++;
            if ((int)markers.size() < eventsLimit) {
                markers.push_back(markerEntry(line, number));
                markerLabels.push_back({data.seconds(line), "M" + std::to_string(number), false});
            }
        }
    }
    out["events"] = events;
    out["markers"] = markers;
    out["events_total"] = eventsTotal;
    out["markers_total"] = markersTotal;
    out["events_truncated"] = eventsTotal > (int)events.size();
    out["markers_truncated"] = markersTotal > (int)markers.size();
    Json clips = Json::array();
    for (const auto& [clip, count] : byClip) clips.push_back({{"clip", clip}, {"plays", count}});
    Json eventStarts = Json::array();
    for (const auto& [event, entry] : byEvent) eventStarts.push_back({{"event", event}, {"event_path", entry.first}, {"starts", entry.second}});
    out["voices"] = {{"started", started}, {"stopped", stopped}, {"stolen", stolen}, {"rejected", rejected}, {"peak", voicesPeak(data, "")},
                     {"by_clip", clips}, {"by_event", eventStarts}};
    out["mixer"] = mixer;
    // the music system's play, section, stinger ... lines (S39 R15), as written
    if (!music.empty()) out["music"] = music;
    return out;
}

AudioSessionReader::Json AudioSessionReader::stopSummary(const AudioSessionData& data) {
    int started = 0, markers = 0;
    for (const Json& line : data.events) {
        const std::string kind = line.value("kind", "");
        if (kind == "voice_start") started++;
        else if (isMarkerKind(kind)) markers++;
    }
    return {{"session", data.capture.id}, {"duration_s", data.capture.session.value("duration_s", 0.0)},
            {"integrated_lufs", dbOrNull(data.loudness.integrated)}, {"momentary_max", dbOrNull(data.loudness.momentaryMax)},
            {"true_peak_dbtp", AudioAnalysis::rounded(data.loudness.truePeakDb, 0.01)}, {"voices_started", started},
            {"voices_peak", data.capture.session.value("voices_peak", 0)}, {"markers", markers},
            {"warnings", data.capture.session.value("warnings", Json::array())}, {"next", "audio_session report"}};
}
