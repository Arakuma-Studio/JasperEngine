// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuiltinSoundImport.h"

#include "AudioProvenance.h"
#include "SoundEssentials.h"
#include "SoundLibrary.h"
#include "mcp/McpValues.h"
#include "util/SHA1.h"
#include "util/Sha256.h"

#include "audio/AudioFileCheck.h"

#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

bool builtinSoundReadBytes(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream bytes;
    bytes << in.rdbuf();
    out = bytes.str();
    return true;
}

} // namespace

std::string BuiltinSoundImport::Clip::reference() const {
    return std::string(library ? SoundLibrary::PREFIX : SoundEssentials::PREFIX) + (entry ? entry->id : std::string());
}

bool BuiltinSoundImport::Clip::read(std::string& bytes, std::string& error) const {
    if (!entry) {
        error = "not a built-in clip";
        return false;
    }
    if (library) return SoundLibrary::get().read(*entry, bytes, error);
    bytes = SoundEssentials::bytes(*entry);
    return true;
}

AudioAnalysis::Labels BuiltinSoundImport::Clip::labels() const {
    return library ? SoundLibrary::labels(*entry) : SoundEssentials::labels(*entry);
}

ClipImportSettings BuiltinSoundImport::Clip::played(unsigned int sampleRate) const {
    const AudioAnalysis::Region region = entry->importRegion(sampleRate);
    ClipImportSettings settings;
    settings.fadeInFrames = region.fadeInFrames;
    settings.fadeOutFrames = region.fadeOutFrames;
    return settings;
}

bool BuiltinSoundImport::isReference(const std::string& text) {
    return SoundEssentials::isReference(text) || SoundLibrary::isReference(text);
}

bool BuiltinSoundImport::resolve(const std::vector<std::string>& refs, std::vector<Clip>& out, std::string& error) {
    out.clear();
    std::set<std::string> seen;
    auto add = [&](bool library, const SoundManifestClip* entry) {
        Clip clip;
        clip.library = library;
        clip.entry = entry;
        if (seen.insert(clip.reference()).second) out.push_back(clip);
    };
    for (const std::string& ref : refs) {
        if (SoundEssentials::isReference(ref)) {
            std::vector<const SoundManifestClip*> clips;
            if (!SoundEssentials::match(ref, clips, error)) return false;
            for (const SoundManifestClip* clip : clips) add(false, clip);
        } else if (SoundLibrary::isReference(ref)) {
            const std::vector<const SoundManifestClip*> clips = SoundLibrary::get().match(ref, error);
            if (clips.empty()) return false;
            for (const SoundManifestClip* clip : clips) add(true, clip);
        } else {
            error = ref + " is not a built-in reference (essentials:<id> or library:<id>)";
            return false;
        }
    }
    if (out.size() > MAX_CLIPS) {
        error = "these references are " + std::to_string(out.size()) + " clips; at most " + std::to_string(MAX_CLIPS) +
                " at once: name the ids, or import a type with library:<type>/* in pages (search_sounds lists them)";
        out.clear();
        return false;
    }
    return true;
}

std::string BuiltinSoundImport::stepName(const std::vector<Clip>& clips) {
    bool essentials = false, library = false;
    for (const Clip& clip : clips) (clip.library ? library : essentials) = true;
    if (library && essentials) return "Import Built-in Sounds";
    return library ? "Import Starter Library Sounds" : "Import Sound Essentials";
}

bool BuiltinSoundImport::analyze(const Clip& clip, const fs::path& cacheDirectory, nlohmann::json& out, std::string& error, AudioAnalysis::Signal* decoded) {
    std::string bytes;
    if (!clip.read(bytes, error)) return false;
    AudioAnalysis::Signal signal;
    nlohmann::json cached;
    const bool known = AudioAnalysis::readCache(cacheDirectory, SHA1::hash(bytes), cached);
    if (known) {
        if (!AudioAnalysis::decode(bytes, signal, error)) return false;
    } else {
        if (!AudioAnalysis::analyzeBytes(bytes, cached, error, &signal)) return false;
        AudioAnalysis::writeCache(cacheDirectory, cached);
    }
    // what an import of it plays
    const ClipImportSettings settings = clip.played(signal.sampleRate);
    if (!AudioAnalysis::analyzePlayed(fs::path(), cacheDirectory, cached, settings, out, error, &signal)) return false;
    if (decoded) {
        *decoded = settings.playsAsFile() ? std::move(signal)
                                          : AudioAnalysis::played(signal, AudioAnalysis::Region::of(settings, (uint32_t)signal.frames));
    }
    return true;
}

bool BuiltinSoundImport::plan(const std::vector<Clip>& clips, const fs::path& directory, Existing existing, Plan& plan, std::string& error) {
    plan = Plan();
    std::set<std::string> seen;
    std::map<std::string, std::string> destinations;
    for (const Clip& clip : clips) {
        if (!clip.entry || !seen.insert(clip.reference()).second) continue;
        const fs::path file = directory / fs::path(clip.entry->id + ".ogg");
        const auto taken = destinations.emplace(file.lexically_normal().generic_string(), clip.reference());
        if (!taken.second) {
            error = file.generic_string() + " is the destination of both " + taken.first->second + " and " + clip.reference();
            return false;
        }
        std::string bytes;
        if (!clip.read(bytes, error)) return false;
        std::error_code ec;
        if (fs::is_directory(file, ec)) {
            error = file.generic_string() + " is a folder";
            return false;
        }
        bool same = false;
        if (fs::exists(file, ec)) {
            std::string current;
            same = builtinSoundReadBytes(file, current) && current == bytes;
            if (!same && existing == Existing::SkipIdentical) {
                plan.conflicts.push_back(file);
                continue;
            }
            if (!same && existing == Existing::RefuseDifferent) {
                error = file.generic_string() + " already exists with other bytes than " + clip.reference() + "; pass overwrite: true to replace it";
                return false;
            }
        }
        if (same && existing != Existing::Replace) {
            plan.skipped.push_back(file);
            continue;
        }
        std::string text;
        if (!clipMetaText(file, mcp::McpValues::jsonToYaml(clip.entry->meta), text, error)) {
            error = clip.reference() + ": " + error;
            return false;
        }
        plan.writes.push_back({file, std::move(bytes)});
        plan.writes.push_back({fs::path(file.string() + ".meta"), std::move(text)});
        plan.imported.push_back(file);
    }
    return true;
}
