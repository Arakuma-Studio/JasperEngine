// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioImportSettings.h"

#include "SoundTypes.h"
#include "command/type/SetAssetMetaCmd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

bool numberOfNode(const YAML::Node& node, double& out) {
    if (!node.IsScalar()) return false;
    try {
        out = node.as<double>();
    } catch (const YAML::Exception&) {
        return false;
    }
    return std::isfinite(out);
}

bool readNumberKey(const YAML::Node& value, const char* key, double& out, std::string& error) {
    if (numberOfNode(value, out)) return true;
    error = std::string(key) + " must be a number";
    return false;
}

bool readTrimKey(const YAML::Node& value, const char* key, AudioImportSettings::Trim& out, std::string& error) {
    if (value.IsScalar() && value.as<std::string>("") == "auto") {
        out.automatic = true;
        out.ms = 0.0;
        return true;
    }
    out.automatic = false;
    if (numberOfNode(value, out.ms)) return true;
    error = std::string(key) + " must be a number of milliseconds or \"auto\"";
    return false;
}

bool isKnownKey(const std::string& key) {
    if (key == "version") return true;
    const std::vector<std::string>& names = AudioImportSettings::keyNames();
    return std::find(names.begin(), names.end(), key) != names.end();
}

std::string keyList() {
    std::string out;
    for (const std::string& name : AudioImportSettings::keyNames()) out += (out.empty() ? "" : ", ") + name;
    return out;
}

int64_t framesOf(double ms, unsigned int rate) {
    return (int64_t)std::llround(ms * (double)rate / 1000.0);
}

} // namespace

const std::vector<std::string>& AudioImportSettings::keyNames() {
    static const std::vector<std::string> names = {"type", "gain_db", "trim_head_ms", "trim_tail_ms", "fade_in_ms", "fade_out_ms",
                                                   "loop_start_ms", "loop_end_ms"};
    return names;
}

bool AudioImportSettings::sameSound(const AudioImportSettings& other) const {
    return gainDb == other.gainDb && trimHead == other.trimHead && trimTail == other.trimTail && fadeInMs == other.fadeInMs &&
           fadeOutMs == other.fadeOutMs && loopStartMs == other.loopStartMs && loopEndMs == other.loopEndMs;
}

bool AudioImportSettings::operator==(const AudioImportSettings& other) const {
    return type == other.type && sameSound(other) && YAML::Dump(unknown) == YAML::Dump(other.unknown);
}

bool AudioImportSettings::isDefault() const {
    return type.empty() && sameSound(AudioImportSettings()) && (!unknown.IsMap() || unknown.size() == 0);
}

bool AudioImportSettings::validate(std::string& error) const {
    auto fail = [&error](const std::string& why) {
        error = why;
        return false;
    };
    if (!type.empty() && !SoundTypes::find(type)) {
        return fail("type \"" + type + "\" is not a sound type (the list: search_sounds' `type`)");
    }
    if (!std::isfinite(gainDb) || gainDb < GAIN_MIN_DB || gainDb > GAIN_MAX_DB) {
        return fail("gain_db must be within " + numberText(GAIN_MIN_DB) + " .. " + numberText(GAIN_MAX_DB));
    }
    if (!trimHead.automatic && (!std::isfinite(trimHead.ms) || trimHead.ms < 0.0)) return fail("trim_head_ms must be >= 0 or \"auto\"");
    if (!trimTail.automatic && (!std::isfinite(trimTail.ms) || trimTail.ms < 0.0)) return fail("trim_tail_ms must be >= 0 or \"auto\"");
    if (!std::isfinite(fadeInMs) || fadeInMs < 0.0 || fadeInMs > FADE_MAX_MS) return fail("fade_in_ms must be within 0 .. 1000");
    if (!std::isfinite(fadeOutMs) || fadeOutMs < 0.0 || fadeOutMs > FADE_MAX_MS) return fail("fade_out_ms must be within 0 .. 1000");
    if (!std::isfinite(loopStartMs) || (loopStartMs < 0.0 && loopStartMs != -1.0)) return fail("loop_start_ms must be >= 0, or -1 for the trimmed start");
    if (!std::isfinite(loopEndMs) || (loopEndMs < 0.0 && loopEndMs != -1.0)) return fail("loop_end_ms must be >= 0, or -1 for the trimmed end");
    if (loopEndMs != -1.0 && loopEndMs <= std::max(loopStartMs, 0.0)) return fail("loop_end_ms must be after loop_start_ms");
    return true;
}

bool AudioImportSettings::parse(const YAML::Node& section, AudioImportSettings& out, std::string& error) {
    out = AudioImportSettings();
    if (!section || section.IsNull()) return true;
    if (!section.IsMap()) {
        error = "import.audioImporter is not a mapping";
        return false;
    }
    for (const auto& entry : section) {
        const std::string key = entry.first.as<std::string>("");
        const YAML::Node value = entry.second;
        bool ok = true;
        if (key == "version") {
            continue;
        } else if (key == "type") {
            if (!value.IsScalar()) {
                error = "type must be a string";
                return false;
            }
            out.type = value.as<std::string>("");
        } else if (key == "gain_db") {
            ok = readNumberKey(value, "gain_db", out.gainDb, error);
        } else if (key == "trim_head_ms") {
            ok = readTrimKey(value, "trim_head_ms", out.trimHead, error);
        } else if (key == "trim_tail_ms") {
            ok = readTrimKey(value, "trim_tail_ms", out.trimTail, error);
        } else if (key == "fade_in_ms") {
            ok = readNumberKey(value, "fade_in_ms", out.fadeInMs, error);
        } else if (key == "fade_out_ms") {
            ok = readNumberKey(value, "fade_out_ms", out.fadeOutMs, error);
        } else if (key == "loop_start_ms") {
            ok = readNumberKey(value, "loop_start_ms", out.loopStartMs, error);
        } else if (key == "loop_end_ms") {
            ok = readNumberKey(value, "loop_end_ms", out.loopEndMs, error);
        } else {
            if (!out.unknown.IsMap()) out.unknown = YAML::Node(YAML::NodeType::Map);
            out.unknown[key] = YAML::Clone(value);
        }
        if (!ok) return false;
    }
    return out.validate(error);
}

bool AudioImportSettings::load(const std::filesystem::path& asset, AudioImportSettings& out, std::string& error) {
    return parse(SetAssetMetaCmd::read(asset, metaKey), out, error);
}

std::string AudioImportSettings::numberText(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.4f", value);
    std::string out = text;
    // the C locale's point: every project file is written before any locale matters, but be sure
    std::replace(out.begin(), out.end(), ',', '.');
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out == "-0" ? "0" : out;
}

YAML::Node AudioImportSettings::toYaml() const {
    if (isDefault()) return YAML::Node(YAML::NodeType::Null);
    YAML::Node node(YAML::NodeType::Map);
    node["version"] = currentVersion;
    if (!type.empty()) node["type"] = type;
    if (gainDb != 0.0) node["gain_db"] = numberText(gainDb);
    if (!trimHead.isDefault()) node["trim_head_ms"] = trimHead.automatic ? std::string("auto") : numberText(trimHead.ms);
    if (!trimTail.isDefault()) node["trim_tail_ms"] = trimTail.automatic ? std::string("auto") : numberText(trimTail.ms);
    if (fadeInMs != 0.0) node["fade_in_ms"] = numberText(fadeInMs);
    if (fadeOutMs != 0.0) node["fade_out_ms"] = numberText(fadeOutMs);
    if (loopStartMs != -1.0) node["loop_start_ms"] = numberText(loopStartMs);
    if (loopEndMs != -1.0) node["loop_end_ms"] = numberText(loopEndMs);
    if (unknown.IsMap()) {
        for (const auto& entry : unknown) node[entry.first.as<std::string>("")] = YAML::Clone(entry.second);
    }
    return node;
}

AudioImportSettings::Json AudioImportSettings::toJson() const {
    Json out = Json::object();
    if (!type.empty()) out["type"] = type;
    if (gainDb != 0.0) out["gain_db"] = gainDb;
    auto trim = [&out](const char* key, const Trim& t) {
        if (t.isDefault()) return;
        if (t.automatic) out[key] = "auto";
        else out[key] = t.ms;
    };
    trim("trim_head_ms", trimHead);
    trim("trim_tail_ms", trimTail);
    if (fadeInMs != 0.0) out["fade_in_ms"] = fadeInMs;
    if (fadeOutMs != 0.0) out["fade_out_ms"] = fadeOutMs;
    if (loopStartMs != -1.0) out["loop_start_ms"] = loopStartMs;
    if (loopEndMs != -1.0) out["loop_end_ms"] = loopEndMs;
    return out;
}

bool AudioImportSettings::apply(const Json& keys, std::string& error) {
    if (!keys.is_object()) {
        error = "settings must be an object with the keys " + keyList();
        return false;
    }
    AudioImportSettings next = *this;
    auto numberKey = [&](const std::string& key, const Json& value, double fallback, double& out) {
        if (value.is_null()) {
            out = fallback;
            return true;
        }
        if (!value.is_number()) {
            error = key + " must be a number (null resets it)";
            return false;
        }
        out = value.get<double>();
        return true;
    };
    auto trimKey = [&](const std::string& key, const Json& value, Trim& out) {
        out = Trim();
        if (value.is_null()) return true;
        if (value.is_string() && value.get<std::string>() == "auto") {
            out.automatic = true;
            return true;
        }
        if (!value.is_number()) {
            error = key + " must be a number of milliseconds or \"auto\" (null resets it)";
            return false;
        }
        out.ms = value.get<double>();
        return true;
    };
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const std::string& key = it.key();
        const Json& value = it.value();
        if (!isKnownKey(key) || key == "version") {
            error = "unknown key `" + key + "`: the keys are " + keyList();
            return false;
        }
        bool ok = true;
        if (key == "type") {
            if (!value.is_null() && !value.is_string()) {
                error = "type must be a string (null resets it)";
                return false;
            }
            next.type = value.is_null() ? std::string() : value.get<std::string>();
        } else if (key == "gain_db") {
            ok = numberKey(key, value, 0.0, next.gainDb);
        } else if (key == "trim_head_ms") {
            ok = trimKey(key, value, next.trimHead);
        } else if (key == "trim_tail_ms") {
            ok = trimKey(key, value, next.trimTail);
        } else if (key == "fade_in_ms") {
            ok = numberKey(key, value, 0.0, next.fadeInMs);
        } else if (key == "fade_out_ms") {
            ok = numberKey(key, value, 0.0, next.fadeOutMs);
        } else if (key == "loop_start_ms") {
            ok = numberKey(key, value, -1.0, next.loopStartMs);
        } else if (key == "loop_end_ms") {
            ok = numberKey(key, value, -1.0, next.loopEndMs);
        }
        if (!ok) return false;
    }
    if (!next.validate(error)) return false;
    *this = next;
    return true;
}

bool AudioImportSettings::resolve(uint64_t fileFrames, unsigned int sampleRate, double headSilenceMs, double tailSilenceMs,
                                  ClipImportSettings& out, std::string& error, Json* resolved) const {
    out = ClipImportSettings();
    if (!validate(error)) return false;
    if (sampleRate == 0) {
        error = "the file has no sample rate";
        return false;
    }
    const int64_t total = (int64_t)fileFrames;
    const int64_t head = std::min(framesOf(trimHead.automatic ? headSilenceMs : trimHead.ms, sampleRate), total);
    const int64_t tail = std::min(framesOf(trimTail.automatic ? tailSilenceMs : trimTail.ms, sampleRate), total);
    if (head + tail >= total && (head > 0 || tail > 0)) {
        error = "the trims leave nothing of the " + numberText(1000.0 * (double)total / sampleRate) + " ms file";
        return false;
    }
    out.gainDb = (float)gainDb;
    out.trimHeadFrames = (uint32_t)head;
    out.trimTailFrames = (uint32_t)tail;
    out.fadeInFrames = (uint32_t)framesOf(fadeInMs, sampleRate);
    out.fadeOutFrames = (uint32_t)framesOf(fadeOutMs, sampleRate);
    out.loopStartFrame = loopStartMs < 0.0 ? -1 : framesOf(loopStartMs, sampleRate);
    out.loopEndFrame = loopEndMs < 0.0 ? -1 : framesOf(loopEndMs, sampleRate);
    const int64_t regionEnd = total - tail;
    const int64_t loopFrom = out.loopStartFrame < 0 ? head : out.loopStartFrame;
    const int64_t loopTo = out.loopEndFrame < 0 ? regionEnd : out.loopEndFrame;
    if ((out.loopStartFrame >= 0 || out.loopEndFrame >= 0) && (loopFrom < head || loopTo > regionEnd || loopTo - loopFrom < LOOP_MIN_FRAMES)) {
        error = "the loop region (" + std::to_string(loopFrom) + " .. " + std::to_string(loopTo) + " frames) must lie inside the trimmed region (" +
                std::to_string(head) + " .. " + std::to_string(regionEnd) + ") and last at least " + std::to_string(LOOP_MIN_FRAMES) + " frames";
        return false;
    }
    if (resolved) {
        *resolved = {{"gain_db", gainDb},
                     {"trim_head_frames", out.trimHeadFrames},
                     {"trim_tail_frames", out.trimTailFrames},
                     {"fade_in_frames", out.fadeInFrames},
                     {"fade_out_frames", out.fadeOutFrames},
                     {"loop_start_frame", out.loopStartFrame},
                     {"loop_end_frame", out.loopEndFrame},
                     {"sample_rate", sampleRate},
                     {"frames", fileFrames}};
    }
    return true;
}
