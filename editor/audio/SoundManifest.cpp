// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundManifest.h"

#include "SoundTypes.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

using Json = nlohmann::json;

const char* const KINDS[] = {SoundTypes::TRANSIENT, SoundTypes::TONAL, SoundTypes::SWELL, SoundTypes::RISER,
                             SoundTypes::GLITCH, SoundTypes::LOOP, SoundTypes::SPEECH};
const char* const SEMANTIC_LABELS[] = {"ok", "weak", "review"};
const char* const LICENCE = "CC0-1.0";

bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool isLowerHex(const std::string& text, size_t length) {
    if (text.size() != length) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// a pack id: kenney.nl's asset names ("impact-sounds")
bool isPackId(const std::string& id) {
    if (id.empty() || id.front() == '-') return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

// "licenses/<name>.txt": one file name of [A-Za-z0-9._-], never "." or "..", so a manifest
// string can only name a file directly under the collection's licenses/ folder
bool isLicencePath(const std::string& path) {
    const std::string folder = "licenses/";
    if (path.compare(0, folder.size(), folder) != 0) return false;
    const std::string name = path.substr(folder.size());
    if (name.size() < 5 || name.front() == '.' || name.compare(name.size() - 4, 4, ".txt") != 0) return false;
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

// UTF-8 characters (code points) of a text
size_t characters(const std::string& text) {
    size_t n = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) n++;
    }
    return n;
}

bool positiveInteger(const Json& value) {
    if (value.is_number_unsigned()) return value.get<uint64_t>() > 0;
    return value.is_number_integer() && value.get<int64_t>() > 0;
}

// Reads `key` of `object` into out: a required text (absent or not a text fails)
bool text(const Json& object, const char* key, std::string& out, std::string& error, const std::string& where, bool nonEmpty = false) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string() || (nonEmpty && it->get<std::string>().empty())) {
        error = where + ": " + key + (nonEmpty ? " is a non-empty text" : " is a text");
        return false;
    }
    out = it->get<std::string>();
    return true;
}

// an optional number: absent or null leaves out empty
bool number(const Json& object, const char* key, std::optional<double>& out, std::string& error, const std::string& where) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return true;
    if (!it->is_number() || !std::isfinite(it->get<double>())) {
        error = where + ": " + key + " is a number or null";
        return false;
    }
    out = it->get<double>();
    return true;
}

bool wholeNumber(const Json& object, const char* key, std::optional<unsigned int>& out, std::string& error, const std::string& where) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return true;
    if (!positiveInteger(*it) || it->get<uint64_t>() > UINT32_MAX) {
        error = where + ": " + key + " is a whole number above 0 or null";
        return false;
    }
    out = (unsigned int)it->get<uint64_t>();
    return true;
}

bool parseMeasured(const Json& value, SoundManifestMeasured& out, std::string& error, const std::string& where) {
    const std::string at = where + " measured";
    if (!value.is_object()) {
        error = at + " is an object";
        return false;
    }
    for (const char* key : {"channels", "sample_rate", "frames"}) {
        if (!value.contains(key) || !positiveInteger(value[key])) {
            error = at + ": " + key + " is a whole number above 0";
            return false;
        }
    }
    if (value["channels"].get<uint64_t>() > 8 || value["sample_rate"].get<uint64_t>() > UINT32_MAX) {
        error = at + ": channels is 1 to 8 and sample_rate a rate in Hz";
        return false;
    }
    out.channels = (unsigned int)value["channels"].get<uint64_t>();
    out.sampleRate = (unsigned int)value["sample_rate"].get<uint64_t>();
    out.frames = value["frames"].get<uint64_t>();
    std::optional<double> duration;
    if (!number(value, "duration_s", duration, error, at)) return false;
    if (!duration) {
        error = at + ": duration_s is a number";
        return false;
    }
    out.durationS = *duration;
    const std::pair<const char*, std::optional<double>*> numbers[] = {
        {"sample_peak_dbfs", &out.samplePeakDbfs}, {"true_peak_dbtp", &out.truePeakDbtp}, {"momentary_max_lufs", &out.momentaryMaxLufs},
        {"integrated_lufs", &out.integratedLufs}, {"head_ms", &out.headMs}, {"tail_ms", &out.tailMs},
        {"seam_step_db", &out.seamStepDb}, {"seam_level_db", &out.seamLevelDb}};
    for (const auto& [key, field] : numbers) {
        if (!number(value, key, *field, error, at)) return false;
    }
    return true;
}

bool parseProcessing(const Json& value, SoundManifestProcessing& out, std::string& error, const std::string& where) {
    const std::string at = where + " processing";
    if (!value.is_object()) {
        error = at + " is an object";
        return false;
    }
    const std::pair<const char*, std::optional<double>*> numbers[] = {
        {"gain_db", &out.gainDb}, {"highpass_hz", &out.highpassHz}, {"lowpass_hz", &out.lowpassHz}, {"fade_in_ms", &out.fadeInMs},
        {"fade_out_ms", &out.fadeOutMs}, {"loop_crossfade_ms", &out.loopCrossfadeMs}, {"quality", &out.quality}};
    for (const auto& [key, field] : numbers) {
        if (!number(value, key, *field, error, at)) return false;
    }
    if (!wholeNumber(value, "resampled_from", out.resampledFrom, error, at)) return false;
    if (!wholeNumber(value, "channels_from", out.channelsFrom, error, at)) return false;
    for (const auto& [key, field] : {std::pair<const char*, std::string*>{"limited_by", &out.limitedBy}, {"preview", &out.preview}}) {
        const auto it = value.find(key);
        if (it == value.end() || it->is_null()) continue;
        if (!it->is_string()) {
            error = at + ": " + key + " is a text or null";
            return false;
        }
        *field = it->get<std::string>();
    }
    return true;
}

bool parseSemantic(const Json& value, SoundManifestSemantic& out, std::string& error, const std::string& where) {
    const std::string at = where + " semantic";
    if (!value.is_object()) {
        error = at + " is an object {label, heard}";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() != "label" && it.key() != "heard") {
            error = at + " has the key " + it.key() + " (only label and heard)";
            return false;
        }
    }
    const Json label = value.value("label", Json());
    if (!label.is_string() || std::find(std::begin(SEMANTIC_LABELS), std::end(SEMANTIC_LABELS), label.get<std::string>()) == std::end(SEMANTIC_LABELS)) {
        error = at + ": label is ok, weak or review";
        return false;
    }
    out.label = label.get<std::string>();
    const Json heard = value.value("heard", Json::array());
    if (!heard.is_array() || heard.size() > SoundManifest::SEMANTIC_MAX_HEARD) {
        error = at + ": heard is a list of at most " + std::to_string(SoundManifest::SEMANTIC_MAX_HEARD) + " texts";
        return false;
    }
    for (const Json& tag : heard) {
        if (!tag.is_string() || tag.get<std::string>().empty() || characters(tag.get<std::string>()) > SoundManifest::SEMANTIC_MAX_TAG_LENGTH) {
            error = at + ": each heard tag is a text of 1 to " + std::to_string(SoundManifest::SEMANTIC_MAX_TAG_LENGTH) + " characters";
            return false;
        }
        out.heard.push_back(tag.get<std::string>());
    }
    return true;
}

bool parsePack(const Json& value, size_t index, SoundManifestPack& out, std::string& error) {
    std::string where = "packs[" + std::to_string(index) + "]";
    if (!value.is_object()) {
        error = where + " is an object";
        return false;
    }
    if (!text(value, "id", out.id, error, where, true)) return false;
    where = "pack \"" + out.id + "\"";
    if (!isPackId(out.id)) {
        error = where + ": id is lowercase letters, digits and hyphens";
        return false;
    }
    if (!text(value, "title", out.title, error, where) || !text(value, "url", out.url, error, where) ||
        !text(value, "landing", out.landing, error, where) || !text(value, "author", out.author, error, where, true) ||
        !text(value, "licence", out.licence, error, where) || !text(value, "licence_file", out.licenceFile, error, where) ||
        !text(value, "sha256", out.sha256, error, where)) {
        return false;
    }
    if (!isLowerHex(out.sha256, 64)) {
        error = where + ": sha256 is 64 lowercase hex digits";
        return false;
    }
    if (!value.contains("bytes") || !positiveInteger(value["bytes"])) {
        error = where + ": bytes is a whole number above 0";
        return false;
    }
    out.bytes = value["bytes"].get<uint64_t>();
    if (out.licence != LICENCE) {
        error = where + ": licence is " + LICENCE + ", not " + out.licence;
        return false;
    }
    if (!isLicencePath(out.licenceFile)) {
        error = where + ": licence_file is licenses/<name>.txt, not " + out.licenceFile;
        return false;
    }
    if (value.contains("credits_file")) {
        if (!text(value, "credits_file", out.creditsFile, error, where)) return false;
        if (!isLicencePath(out.creditsFile)) {
            error = where + ": credits_file is licenses/<name>.txt, not " + out.creditsFile;
            return false;
        }
    }
    out.raw = value;
    return true;
}

bool parseClip(const Json& value, size_t index, bool library, const SoundManifest& manifest, SoundManifestClip& out, std::string& error) {
    std::string where = "clips[" + std::to_string(index) + "]";
    if (!value.is_object()) {
        error = where + " is an object";
        return false;
    }
    if (!text(value, "id", out.id, error, where, true)) return false;
    where = "clip \"" + out.id + "\"";
    if (!SoundManifest::isValidId(out.id)) {
        error = where + ": the id is <group>/<name>, lowercase letters, digits and _ (a name of at most " +
                std::to_string(SoundManifest::MAX_NAME_LENGTH) + ")";
        return false;
    }
    out.group = out.id.substr(0, out.id.find('/'));
    if (!text(value, "path", out.path, error, where)) return false;
    if (out.path != out.id + ".ogg") {
        error = where + ": path is " + out.id + ".ogg, not " + out.path;
        return false;
    }
    if (!text(value, "sha256", out.sha256, error, where)) return false;
    if (!isLowerHex(out.sha256, 64)) {
        error = where + ": sha256 is 64 lowercase hex digits";
        return false;
    }
    if (!value.contains("bytes") || !positiveInteger(value["bytes"])) {
        error = where + ": bytes is a whole number above 0";
        return false;
    }
    out.bytes = value["bytes"].get<uint64_t>();
    if (!text(value, "type", out.type, error, where)) return false;
    if (!SoundTypes::find(out.type)) {
        error = where + ": type " + out.type + " is not one of the 31 sound types";
        return false;
    }
    if (library && out.type != out.group) {
        error = where + ": type " + out.type + " differs from the id's group " + out.group;
        return false;
    }
    if (!text(value, "kind", out.kind, error, where)) return false;
    if (!SoundManifest::isKind(out.kind)) {
        error = where + ": kind " + out.kind + " is not one of transient, tonal, swell, riser, glitch, loop, speech";
        return false;
    }
    const Json tags = value.value("tags", Json());
    if (!tags.is_array()) {
        error = where + ": tags is a list of texts";
        return false;
    }
    for (const Json& tag : tags) {
        if (!tag.is_string()) {
            error = where + ": tags is a list of texts";
            return false;
        }
        out.tags.push_back(tag.get<std::string>());
    }
    if (!text(value, "description", out.description, error, where) || !text(value, "licence", out.licence, error, where) ||
        !text(value, "source", out.source, error, where) || !text(value, "author", out.author, error, where, true) ||
        !text(value, "url", out.url, error, where)) {
        return false;
    }
    if (out.licence != LICENCE) {
        error = where + ": licence is " + LICENCE + ", not " + out.licence;
        return false;
    }
    auto nullOrAbsent = [&](const char* key) {
        const auto it = value.find(key);
        return it == value.end() || it->is_null();
    };
    if (out.source == "kenney") {
        if (!text(value, "pack", out.pack, error, where, true) || !text(value, "source_file", out.sourceFile, error, where, true)) return false;
        if (!manifest.findPack(out.pack)) {
            error = where + ": pack " + out.pack + " is not one of the manifest's packs";
            return false;
        }
        if (!nullOrAbsent("source_id") || !nullOrAbsent("licence_url")) {
            error = where + ": a kenney clip has no source_id or licence_url";
            return false;
        }
    } else if (out.source == "freesound") {
        if (!nullOrAbsent("pack") || !nullOrAbsent("source_file")) {
            error = where + ": a freesound clip's pack and source_file are null";
            return false;
        }
        if (!value.contains("source_id") || !positiveInteger(value["source_id"]) || value["source_id"].get<uint64_t>() > (uint64_t)INT64_MAX) {
            error = where + ": source_id is the Freesound sound id, a whole number above 0";
            return false;
        }
        out.sourceId = (int64_t)value["source_id"].get<uint64_t>();
        if (!text(value, "licence_url", out.licenceUrl, error, where, true)) return false;
    } else {
        error = where + ": source is kenney or freesound, not " + out.source;
        return false;
    }
    if (value.contains("attribution") && !value["attribution"].is_null() && !text(value, "attribution", out.attribution, error, where)) return false;
    if (!value.contains("measured") || !parseMeasured(value["measured"], out.measured, error, where)) {
        if (!value.contains("measured")) error = where + ": measured is required";
        return false;
    }
    if (value.contains("processing") && !value["processing"].is_null()) {
        SoundManifestProcessing processing;
        if (!parseProcessing(value["processing"], processing, error, where)) return false;
        out.processing = processing;
    } else if (library) {
        error = where + ": processing is required in the starter library";
        return false;
    }
    if (!value.contains("meta") || !value["meta"].is_object()) {
        error = where + ": meta is required, the .meta sections an import writes";
        return false;
    }
    out.meta = value["meta"];
    for (auto it = out.meta.begin(); it != out.meta.end(); ++it) {
        if (it.key() != "import" && it.key() != "provenance" && it.key() != "labels") {
            error = where + " meta: has the section " + it.key() + " (only import, provenance and labels)";
            return false;
        }
    }
    {
        const Json importSection = out.meta.value("import", Json::object());
        std::string importError;
        if (!importSection.is_object() || !SoundManifestImport::parse(importSection.value("audioImporter", Json()), out.importDefaults, importError)) {
            error = where + " meta import.audioImporter: " + importError;
            return false;
        }
    }
    if (value.contains("semantic") && !value["semantic"].is_null()) {
        SoundManifestSemantic semantic;
        if (!parseSemantic(value["semantic"], semantic, error, where)) return false;
        out.semantic = semantic;
    }
    out.raw = value;
    return true;
}

} // namespace

// ======================================================================== SoundManifestImport

Json SoundManifestImport::toJson() const {
    Json out = Json::object();
    if (fadeInMs) out["fade_in_ms"] = *fadeInMs;
    if (fadeOutMs) out["fade_out_ms"] = *fadeOutMs;
    return out;
}

AudioAnalysis::Region SoundManifestImport::region(unsigned int sampleRate) const {
    auto frames = [&](const std::optional<double>& ms) {
        if (!ms) return (uint32_t)0;
        return (uint32_t)std::min(std::round(*ms * (double)sampleRate / 1000.0), (double)UINT32_MAX);
    };
    AudioAnalysis::Region region;
    region.fadeInFrames = frames(fadeInMs);
    region.fadeOutFrames = frames(fadeOutMs);
    return region;
}

bool SoundManifestImport::parse(const Json& value, SoundManifestImport& out, std::string& error) {
    out = SoundManifestImport();
    if (value.is_null()) return true;
    if (!value.is_object()) {
        error = "an object {fade_in_ms, fade_out_ms}";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() != "fade_in_ms" && it.key() != "fade_out_ms") continue;
        const Json& ms = it.value();
        if (!ms.is_number() || !std::isfinite(ms.get<double>()) || ms.get<double>() < 0.0 || ms.get<double>() > (double)CLIP_FADE_MAX_MS) {
            error = it.key() + " is a number of milliseconds from 0 to 1000";
            return false;
        }
        (it.key() == "fade_in_ms" ? out.fadeInMs : out.fadeOutMs) = ms.get<double>();
    }
    return true;
}

// ======================================================================== SoundManifest

bool SoundManifest::isValidId(const std::string& id) {
    const size_t slash = id.find('/');
    if (slash == std::string::npos || slash == 0 || id.find('/', slash + 1) != std::string::npos) return false;
    const size_t name = id.size() - slash - 1;
    if (name < 1 || name > MAX_NAME_LENGTH) return false;
    for (size_t i = 0; i < id.size(); i++) {
        if (i != slash && !isNameChar(id[i])) return false;
    }
    return true;
}

bool SoundManifest::isKind(const std::string& kind) {
    return std::find_if(std::begin(KINDS), std::end(KINDS), [&](const char* k) { return kind == k; }) != std::end(KINDS);
}

const SoundManifestClip* SoundManifest::find(const std::string& id) const {
    const auto it = std::lower_bound(clips.begin(), clips.end(), id, [](const SoundManifestClip& c, const std::string& key) { return c.id < key; });
    return it != clips.end() && it->id == id ? &*it : nullptr;
}

const SoundManifestPack* SoundManifest::findPack(const std::string& id) const {
    for (const SoundManifestPack& pack : packs) {
        if (pack.id == id) return &pack;
    }
    return nullptr;
}

bool SoundManifest::parse(const std::string& text, const std::string& expectedFormat, SoundManifest& out, std::string& error) {
    out = SoundManifest();
    error.clear();
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        error = "not valid JSON";
        return false;
    }
    if (!root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    const Json format = root.value("format", Json());
    if (!format.is_string() || format.get<std::string>() != expectedFormat) {
        error = "format is " + (format.is_string() ? format.get<std::string>() : format.dump()) + ", not " + expectedFormat;
        return false;
    }
    out.format = format.get<std::string>();
    const Json version = root.value("version", Json());
    if (!positiveInteger(version) || version.get<uint64_t>() > (uint64_t)INT32_MAX) {
        error = "version is a whole number above 0";
        return false;
    }
    out.version = (int)version.get<uint64_t>();
    {
        const int wanted = expectedFormat == LIBRARY_FORMAT ? LIBRARY_VERSION : ESSENTIALS_VERSION;
        if (out.version < wanted) {
            error = "version " + std::to_string(out.version) + " is older than " + std::to_string(wanted) + " (migrate it: tools/sound-library/build_library.py migrate-manifest)";
            return false;
        }
    }
    for (const auto& [key, field] : {std::pair<const char*, std::string*>{"licence", &out.licence}, {"generated_by", &out.generatedBy}}) {
        const auto it = root.find(key);
        if (it == root.end()) continue;
        if (!it->is_string()) {
            error = std::string(key) + " is a text";
            return false;
        }
        *field = it->get<std::string>();
    }
    const bool library = expectedFormat == LIBRARY_FORMAT;

    const Json packs = root.value("packs", Json());
    if (!packs.is_array()) {
        error = "packs is a list";
        return false;
    }
    std::set<std::string> packIds;
    for (size_t i = 0; i < packs.size(); i++) {
        SoundManifestPack pack;
        if (!parsePack(packs[i], i, pack, error)) return false;
        if (!packIds.insert(pack.id).second) {
            error = "pack \"" + pack.id + "\" is listed twice";
            return false;
        }
        out.packs.push_back(std::move(pack));
    }

    const Json clips = root.value("clips", Json());
    if (!clips.is_array()) {
        error = "clips is a list";
        return false;
    }
    std::set<std::string> ids;
    out.clips.reserve(clips.size());
    for (size_t i = 0; i < clips.size(); i++) {
        SoundManifestClip clip;
        if (!parseClip(clips[i], i, library, out, clip, error)) return false;
        if (!ids.insert(clip.id).second) {
            error = "clip \"" + clip.id + "\" is listed twice";
            return false;
        }
        out.clips.push_back(std::move(clip));
    }
    std::sort(out.clips.begin(), out.clips.end(), [](const SoundManifestClip& a, const SoundManifestClip& b) { return a.id < b.id; });
    out.raw = std::move(root);
    return true;
}
