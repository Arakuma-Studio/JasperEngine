// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioImportRegistry.h"

#include "AudioAnalysis.h"
#include "AudioImportSettings.h"
#include "Log.h"
#include "Project.h"
#include "asset/AssetTypes.h"
#include "audio/AudioFileCheck.h"
#include "command/type/SetAssetMetaCmd.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

bool isAudioFile(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3" || extension == ".flac";
}

bool readWhole(const fs::path& file, std::string& bytes) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? (std::streamoff)in.tellg() : -1;
    if (size < 0) return false;
    bytes.assign((size_t)size, '\0');
    in.seekg(0);
    return size == 0 || (bool)in.read(&bytes[0], size);
}

} // namespace

bool AudioImportRegistry::resolve(const Project& project, const fs::path& asset, ClipImportSettings& out, std::string& error) {
    out = ClipImportSettings();
    AudioImportSettings settings;
    if (!AudioImportSettings::load(asset, settings, error)) return false;
    if (settings.isDefault()) return true;
    std::string bytes;
    if (!readWhole(asset, bytes)) {
        error = "can't read the file";
        return false;
    }
    AudioFileFormat format;
    if (!AudioFileCheck::prepare(reinterpret_cast<unsigned char*>(&bytes[0]), bytes.size(), AudioDecode::Stream, format, error)) return false;
    double headMs = 0.0, tailMs = 0.0;
    if (settings.trimHead.automatic || settings.trimTail.automatic) {
        AudioAnalysis::Json silence;
        if (!AudioAnalysis::silenceOfFile(asset, bytes, AudioAnalysis::cacheDirectory(project.getProjectPath()), silence, error)) return false;
        headMs = silence.value("head_ms", 0.0);
        tailMs = silence.value("tail_ms", 0.0);
    }
    return settings.resolve(format.frames, format.sampleRate, headMs, tailMs, out, error);
}

std::map<std::string, ClipImportSettings> AudioImportRegistry::collect(const Project& project, std::vector<std::string>& problems) {
    std::map<std::string, ClipImportSettings> clips;
    const fs::path assets = project.getAssetsPath();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(assets, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& path = it->path();
        std::error_code entryEc;
        if (it->is_directory(entryEc)) {
            const std::string name = path.filename().string();
            if (!name.empty() && name[0] == '.') it.disable_recursion_pending();
            continue;
        }
        // only a meta names settings: an audio file without one is not even opened
        if (!AssetTypes::isMetaFile(path)) continue;
        fs::path asset = path;
        asset.replace_extension();
        if (!isAudioFile(asset) || !fs::is_regular_file(asset, entryEc)) continue;
        ClipImportSettings settings;
        std::string error;
        const std::string relative = FileUtils::pathToGenericUtf8(fs::relative(asset, assets, entryEc));
        if (!resolve(project, asset, settings, error)) {
            problems.push_back(relative + ": " + error);
            continue;
        }
        if (!settings.isDefault()) clips[relative] = settings;
    }
    return clips;
}

void AudioImportRegistry::registerProject(const Project& project) {
    std::vector<std::string> problems;
    const std::map<std::string, ClipImportSettings> clips = collect(project, problems);
    AudioImportTable::clear();
    for (const auto& [clip, settings] : clips) AudioImportTable::set(clip, settings);
    for (const std::string& problem : problems) Log::warn("Import settings not applied: %s", problem.c_str());
}

void AudioImportRegistry::filesChanged(const Project& project, const std::vector<fs::path>& changed) {
    const fs::path assets = project.getAssetsPath();
    for (const fs::path& path : changed) {
        fs::path asset = path;
        if (AssetTypes::isMetaFile(path)) asset.replace_extension();
        if (!isAudioFile(asset) || !project.isInsideAssetsPath(asset)) continue;
        std::error_code ec;
        const std::string relative = FileUtils::pathToGenericUtf8(fs::relative(asset, assets, ec));
        ClipImportSettings settings;
        std::string error;
        if (!fs::is_regular_file(asset, ec)) {
            AudioImportTable::set(relative, ClipImportSettings());
        } else if (resolve(project, asset, settings, error)) {
            AudioImportTable::set(relative, settings);
        } else {
            Log::warn("Import settings not applied: %s: %s", relative.c_str(), error.c_str());
            AudioImportTable::set(relative, ClipImportSettings());
        }
    }
}

std::string AudioImportRegistry::exportText(const std::map<std::string, ClipImportSettings>& clips) {
    if (clips.empty()) return std::string();
    nlohmann::json entries = nlohmann::json::object();
    const ClipImportSettings defaults;
    for (const auto& [clip, s] : clips) {
        nlohmann::json entry = nlohmann::json::object();
        if (s.gainDb != defaults.gainDb) entry["gain_db"] = s.gainDb;
        if (s.trimHeadFrames != 0) entry["trim_head_frames"] = s.trimHeadFrames;
        if (s.trimTailFrames != 0) entry["trim_tail_frames"] = s.trimTailFrames;
        if (s.fadeInFrames != 0) entry["fade_in_frames"] = s.fadeInFrames;
        if (s.fadeOutFrames != 0) entry["fade_out_frames"] = s.fadeOutFrames;
        if (s.loopStartFrame != -1) entry["loop_start_frame"] = s.loopStartFrame;
        if (s.loopEndFrame != -1) entry["loop_end_frame"] = s.loopEndFrame;
        if (s.bpm != defaults.bpm) entry["bpm"] = s.bpm;
        if (s.beatsPerBar != defaults.beatsPerBar) entry["beats_per_bar"] = s.beatsPerBar;
        if (s.firstBeatFrame != 0) entry["first_beat_frame"] = s.firstBeatFrame;
        entries[clip] = std::move(entry);
    }
    return nlohmann::json{{"format", AudioImportTable::FORMAT}, {"version", AudioImportTable::VERSION}, {"clips", std::move(entries)}}.dump(1) + "\n";
}
