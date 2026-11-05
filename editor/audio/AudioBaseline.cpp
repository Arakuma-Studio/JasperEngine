// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioBaseline.h"

#include "AudioQa.h"

#include <fstream>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

fs::path AudioBaseline::file(const fs::path& projectDirectory, const std::string& name) {
    return projectDirectory / "baselines" / "audio" / (name + ".json");
}

bool AudioBaseline::isName(const std::string& name) {
    return !name.empty() && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..";
}

AudioBaseline::Json AudioBaseline::build(const AudioSessionData& data, double marginDb) {
    Json out = {{"format", "jasperengine.audio-baseline"}, {"version", 1}, {"session", data.capture.id},
                {"scene", data.capture.session.value("scene", "")}};
    Json starts = Json::array(), markers = Json::array();
    std::map<std::string, int> startCounts, markerCounts;
    for (const Json& line : data.events) {
        if (!data.inRange(line)) continue;
        const std::string kind = line.value("kind", "");
        if (kind == "voice_start") {
            const std::string key = line.value("clip", "") + "|" + (line.contains("entity") && line["entity"].is_string() ? line["entity"].get<std::string>() : std::string());
            const int64_t stamp = (int64_t)line.value("sample", (uint64_t)0) - (int64_t)data.startFrame;
            const int64_t onset = AudioQa::onsetFrame(data, stamp, data.rate, marginDb);
            starts.push_back({{"key", key}, {"n", startCounts[key]++}, {"t", line.value("t", 0.0)},
                              {"onset_ms", onset < 0 ? Json(nullptr) : Json((double)(onset - stamp) * 1000.0 / data.rate)}});
        } else if (AudioSessionReader::isMarkerKind(kind) && kind != "input") {
            const std::string key = kind + "|" + line.value("label", "");
            // the 100 ms line that holds it
            Json bands = Json::object();
            for (const Json& level : data.levels) {
                if (level.value("sample", (uint64_t)0) <= line.value("sample", (uint64_t)0)) continue;
                const Json groups = level.value("groups", Json::object());
                for (auto it = groups.begin(); it != groups.end(); ++it) bands[it.key()] = it.value().value("bands_db", Json::array());
                break;
            }
            markers.push_back({{"key", key}, {"n", markerCounts[key]++}, {"bands_db", bands}});
        }
    }
    out["starts"] = starts;
    out["markers"] = markers;
    return out;
}

bool AudioBaseline::read(const fs::path& file, Json& out, std::string& why) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        why = "no baseline at " + file.generic_string() + ": save one with save_baseline";
        return false;
    }
    out = Json::parse(in, nullptr, false);
    if (out.is_discarded() || !out.is_object() || out.value("format", "") != "jasperengine.audio-baseline") {
        why = file.generic_string() + " is not a jasperengine.audio-baseline";
        return false;
    }
    return true;
}
