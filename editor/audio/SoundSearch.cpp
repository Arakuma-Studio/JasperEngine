// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "audio/SoundEventIO.h"
#include "organisation/AssetLabels.h"
#include "util/ProjectChecks.h"
#include "SoundSearch.h"

#include "AudioAnalysis.h"
#include "AudioImportSettings.h"
#include "SoundEssentials.h"
#include "SoundLibrary.h"
#include "SoundTypes.h"

#include "Project.h"
#include "util/ProjectChecks.h"
#include "util/SHA1.h"
#include "util/Sha256.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

using Json = SoundSearch::Json;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(lower(text));
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

bool contains(const std::string& haystack, const std::string& word) {
    return lower(haystack).find(word) != std::string::npos;
}

// the words of a text (runs of letters and digits), lower case
std::vector<std::string> tokens(const std::string& text) {
    std::vector<std::string> out;
    std::string word;
    for (char c : lower(text)) {
        if (std::isalnum((unsigned char)c)) {
            word += c;
        } else if (!word.empty()) {
            out.push_back(word);
            word.clear();
        }
    }
    if (!word.empty()) out.push_back(word);
    return out;
}

// a text's words folded to their singular, each between spaces: "sounds/footsteps.wav" -> " sound footstep wav "
std::string foldedText(const std::string& text) {
    std::string out = " ";
    for (const std::string& word : tokens(text)) out += SoundSearch::folded(word) + " ";
    return out;
}

// a query word (lower case) in a field: as typed, its singular, or its singular among the field's
// folded words ("body" in "bodies")
bool matches(const std::string& field, const std::string& word, const std::string& singular) {
    if (field.empty()) return false;
    const std::string text = lower(field);
    if (text.find(word) != std::string::npos) return true;
    if (singular != word && text.find(singular) != std::string::npos) return true;
    return foldedText(field).find(singular) != std::string::npos;
}

// a query word (lower case) as a whole word of a heard tag: as
// typed, or its singular among the tag's folded words ("knocks" in "wooden knock"). Whole words, so
// a model's guess never matches inside another word ("rain" in "training").
bool heardMatches(const std::string& tag, const std::string& word, const std::string& singular) {
    for (const std::string& token : tokens(tag)) {
        if (token == word || token == singular || SoundSearch::folded(token) == singular) return true;
    }
    return false;
}

bool heardExactly(const std::string& tag, const std::string& word) {
    const std::vector<std::string> own = tokens(tag);
    return std::find(own.begin(), own.end(), word) != own.end();
}

// a query word anywhere in an entry (search's fields)
bool entryHas(const SoundSearch::Entry& entry, const std::string& word, const std::string& singular) {
    for (const std::string& tag : entry.tags) {
        if (matches(tag, word, singular)) return true;
    }
    for (const std::string& tag : entry.heardTags) {
        if (heardMatches(tag, word, singular)) return true;
    }
    return matches(entry.name, word, singular) || matches(entry.path, word, singular) || matches(entry.type, word, singular) ||
           matches(entry.description, word, singular);
}

// a source's place in the order of ties: project, essentials, starter library
int sourceRank(const std::string& source) {
    if (source == "project") return 0;
    if (source == "essentials") return 1;
    return 2;
}

// an entry of the scope's sources
bool inScopeOf(SoundSearch::Scope scope, const SoundSearch::Entry& entry) {
    switch (scope) {
        case SoundSearch::Scope::Project: return entry.source == "project";
        case SoundSearch::Scope::Essentials: return entry.source == "essentials";
        case SoundSearch::Scope::Library: return entry.source == "library";
        case SoundSearch::Scope::All: return true;
    }
    return true;
}

// Levenshtein distance, cut off above limit (limit + 1 then)
size_t editDistance(const std::string& a, const std::string& b, size_t limit) {
    const size_t n = a.size(), m = b.size();
    if ((n > m ? n - m : m - n) > limit) return limit + 1;
    std::vector<size_t> previous(m + 1), current(m + 1);
    for (size_t j = 0; j <= m; j++) previous[j] = j;
    for (size_t i = 1; i <= n; i++) {
        current[0] = i;
        size_t best = current[0];
        for (size_t j = 1; j <= m; j++) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
            best = std::min(best, current[j]);
        }
        if (best > limit) return limit + 1;
        std::swap(previous, current);
    }
    return std::min(previous[m], limit + 1);
}

double numberOr(const Json& value, double fallback) {
    return value.is_number() ? value.get<double>() : fallback;
}

bool readBytes(const fs::path& file, std::string& bytes) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? (std::streamoff)in.tellg() : -1;
    if (size < 0) return false;
    bytes.assign((size_t)size, '\0');
    in.seekg(0);
    return size == 0 || (bool)in.read(&bytes[0], size);
}

// The hashes of a file by path, size and modification time, so an unchanged file is read once
// per session: its SHA-1 (the analysis cache's key) and, asked only for a file as large as an
// essential, its SHA-256
struct FileHashes {
    uintmax_t size = 0;
    fs::file_time_type mtime;
    std::string sha1;
    std::string sha256;
};
std::mutex hashMutex;
std::unordered_map<std::string, FileHashes> fileHashes;

bool knownHashes(const fs::path& file, uintmax_t size, fs::file_time_type mtime, FileHashes& out) {
    std::lock_guard<std::mutex> lock(hashMutex);
    auto it = fileHashes.find(file.string());
    if (it == fileHashes.end() || it->second.size != size || it->second.mtime != mtime) return false;
    out = it->second;
    return true;
}

void rememberHashes(const fs::path& file, const FileHashes& hashes) {
    std::lock_guard<std::mutex> lock(hashMutex);
    fileHashes[file.string()] = hashes;
}

// the essentials' SHA-1s, by id (their bytes never change)
const std::string& essentialSha1(const SoundManifestClip& clip) {
    static std::mutex mutex;
    static std::map<std::string, std::string> hashes;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = hashes.find(clip.id);
    if (it == hashes.end()) it = hashes.emplace(clip.id, SHA1::hash(SoundEssentials::bytes(clip))).first;
    return it->second;
}

// The SHA-1 of library bytes whose SHA-256 is known (SoundLibrary::read checked them against the
// manifest), so a later search finds the cached analysis without reading the file again
std::mutex librarySha1Mutex;
std::map<std::string, std::string> librarySha1;

bool knownLibrarySha1(const std::string& sha256, std::string& sha1) {
    std::lock_guard<std::mutex> lock(librarySha1Mutex);
    auto it = librarySha1.find(sha256);
    if (it == librarySha1.end()) return false;
    sha1 = it->second;
    return true;
}

void rememberLibrarySha1(const std::string& sha256, const std::string& sha1) {
    std::lock_guard<std::mutex> lock(librarySha1Mutex);
    librarySha1[sha256] = sha1;
}

// The cached analysis of bytes (analysed and cached on a miss, counted)
bool cachedAnalysis(const std::string& bytes, const std::string& sha1, const fs::path& cacheDirectory, Json& cached, int& analysed,
                    std::string& error) {
    if (AudioAnalysis::readCache(cacheDirectory, sha1, cached)) return true;
    if (!AudioAnalysis::analyzeBytes(bytes, cached, error)) return false;
    AudioAnalysis::writeCache(cacheDirectory, cached);
    analysed++;
    return true;
}

// the features of an entry from its cached analysis
void setFeatures(SoundSearch::Entry& entry, const Json& cached, const AudioAnalysis::Labels& labels) {
    entry.analysed = true;
    entry.error.clear();
    entry.sha1 = cached.value("sha1", std::string());
    const Json format = cached.value("format", Json::object());
    entry.durationMs = numberOr(format.value("duration_s", Json(0.0)), 0.0) * 1000.0;
    const Json loudness = cached.value("loudness", Json::object());
    entry.lufs = numberOr(loudness.value("momentary_max", Json(nullptr)), NAN);
    const Json peak = cached.value("peak", Json::object());
    entry.truePeakDbtp = numberOr(peak.value("true_peak_dbtp", Json(nullptr)), NAN);
    entry.centroidHz = numberOr(cached.value("centroid_hz", Json(nullptr)), NAN);
    entry.flatness = numberOr(cached.value("flatness", Json(nullptr)), NAN);
    const Json onset = cached.value("onset", Json::object());
    entry.attackMs = numberOr(onset.value("attack_ms", Json(nullptr)), 0.0);
    const Json pitch = cached.value("pitch", Json::object());
    entry.pitchConfidence = numberOr(pitch.value("confidence", Json(nullptr)), 0.0);
    const Json share = cached.value("band_share", Json::object());
    for (int b = 0; b < AudioMeter::BANDS; b++) {
        entry.bandShare[b] = numberOr(share.value(AudioMeter::bandLabel(b), Json(0.0)), 0.0);
    }
    std::string kind, kindSource;
    AudioAnalysis::resolveKind(cached.value("measured_kind", std::string(SoundTypes::TRANSIENT)), labels, kind, kindSource);
    entry.soundKind = kind;
}

void setLabels(SoundSearch::Entry& entry, const AudioAnalysis::Labels& labels) {
    entry.type = labels.type;
    entry.tags.clear();
    if (labels.tags.is_array()) {
        for (const Json& tag : labels.tags) {
            if (tag.is_string()) entry.tags.push_back(tag.get<std::string>());
        }
    }
    entry.licence = labels.licence.is_object() ? labels.licence : Json(nullptr);
}

// Every audio file under the assets directory, linked folders once each by where they lead, as
// the clip import index scans them; in path order
std::vector<fs::path> audioFiles(const fs::path& assets) {
    std::vector<fs::path> out;
    std::set<std::string> visited;
    std::vector<fs::path> pending = {assets};
    std::error_code ec;
    while (!pending.empty()) {
        const fs::path dir = pending.back();
        pending.pop_back();
        const fs::path real = fs::weakly_canonical(dir, ec);
        if (!visited.insert((ec ? dir : real).string()).second) continue;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path path = it->path();
            const std::string name = path.filename().string();
            if (name.empty() || name[0] == '.') continue;
            std::error_code statError;
            if (it->is_directory(statError)) {
                if (!ProjectChecks::isSkippedDir(path)) pending.push_back(path);
            } else if (it->is_regular_file(statError) && Util::isAudioFile(path.string())) {
                out.push_back(path);
            }
        }
        ec.clear();
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string relativeTo(const fs::path& file, const fs::path& root) {
    return file.lexically_normal().lexically_relative(root.lexically_normal()).generic_string();
}

// the project-relative path of an assets-relative one
} // namespace

bool SoundSearch::scopeFromName(const std::string& name, Scope& scope) {
    if (name == "project") scope = Scope::Project;
    else if (name == "essentials") scope = Scope::Essentials;
    else if (name == "library") scope = Scope::Library;
    else if (name == "all") scope = Scope::All;
    else return false;
    return true;
}

const char* SoundSearch::scopeName(Scope scope) {
    switch (scope) {
        case Scope::Project: return "project";
        case Scope::Essentials: return "essentials";
        case Scope::Library: return "library";
        case Scope::All: return "all";
    }
    return "all";
}

SoundSearch::Sources SoundSearch::sources(const Project* project, Scope scope) {
    Sources out;
    out.scope = scope;
    if (!project) return out;
    out.projectDirectory = project->getProjectPath();
    out.assetsDirectory = project->getAssetsPath();
    out.cacheDirectory = AudioAnalysis::cacheDirectory(project->getProjectPath());
    if (scope == Scope::Project || scope == Scope::All) {
        // an event's GUID is its .meta's, which only the database reads on this thread
        for (const fs::path& file : ProjectChecks::findProjectFiles(project->getProjectPath().lexically_normal(), {".soundevent"})) {
            out.events.push_back({file, project->getAssetDatabase().getAssetGuid(file.lexically_relative(project->getProjectPath().lexically_normal())).str()});
        }
    }
    return out;
}

std::vector<SoundSearch::Entry> SoundSearch::index(const Sources& sources, int& analysed, const std::atomic<bool>* cancel,
                                                   const std::function<void(size_t, size_t)>& progress) {
    analysed = 0;
    std::vector<Entry> out;
    const bool wantProject = sources.scope == Scope::Project || sources.scope == Scope::All;
    const bool wantEssentials = sources.scope == Scope::Essentials || sources.scope == Scope::All;
    const bool wantLibrary = sources.scope == Scope::Library || sources.scope == Scope::All;
    const SoundLibrary& library = SoundLibrary::get();
    static const std::vector<SoundManifestClip> noClips;
    const std::vector<SoundManifestClip>& libraryClips = wantLibrary || !sources.referenceLibrary.empty() ? library.all() : noClips;

    // the built-in clips' sizes: a project file of another size can't hold one's bytes
    std::set<uintmax_t> builtinSizes;
    for (const SoundManifestClip& clip : SoundEssentials::all()) builtinSizes.insert(clip.bytes);
    for (const SoundManifestClip& clip : libraryClips) builtinSizes.insert(clip.bytes);

    std::vector<fs::path> files;
    if (!sources.assetsDirectory.empty()) files = audioFiles(sources.assetsDirectory);
    const fs::path reference = sources.referenceFile.empty() ? fs::path() : sources.referenceFile.lexically_normal();
    if (!reference.empty() && std::find(files.begin(), files.end(), reference) == files.end() && Util::isAudioFile(reference.string())) {
        // a reference outside the assets directory (or the scope) is indexed all the same
        files.push_back(reference);
    }
    const size_t total = (wantProject ? files.size() : (reference.empty() ? 0 : 1)) +
                         (!wantEssentials && !sources.referenceEssential.empty() ? 1 : 0) +
                         (wantEssentials ? SoundEssentials::all().size() : 0) +
                         (!wantLibrary && !sources.referenceLibrary.empty() ? 1 : 0) + (wantLibrary ? libraryClips.size() : 0);
    size_t done = 0;
    auto step = [&]() {
        done++;
        if (progress) progress(done, total);
    };

    // -- project clips (and the SHA-256s of the files a built-in clip could be)
    std::set<std::string> projectSha256;
    std::vector<Entry> clips;
    for (const fs::path& file : files) {
        if (cancel && *cancel) return out;
        const bool isReference = !reference.empty() && file.lexically_normal() == reference;
        std::error_code ec;
        const uintmax_t size = fs::file_size(file, ec);
        if (ec) continue;
        const fs::file_time_type mtime = fs::last_write_time(file, ec);
        const bool builtinSized = builtinSizes.count(size) > 0;
        const bool indexed = wantProject || isReference;
        if (!indexed && !builtinSized) continue;

        Entry entry;
        entry.source = "project";
        entry.kind = "clip";
        entry.file = file.lexically_normal();
        entry.path = relativeTo(file, sources.projectDirectory);
        const fs::path fromAssets = file.lexically_normal().lexically_relative(sources.assetsDirectory.lexically_normal());
        entry.id = fromAssets.empty() || *fromAssets.begin() == ".." ? std::string() : fromAssets.generic_string();
        entry.name = file.stem().string();
        entry.inProject = true;

        FileHashes hashes;
        std::string bytes;
        const bool known = knownHashes(file, size, mtime, hashes);
        const bool needSha256 = builtinSized && hashes.sha256.empty();
        if (!known || needSha256) {
            if (!readBytes(file, bytes)) {
                entry.error = "can't read the file";
                if (indexed) clips.push_back(entry);
                if (indexed) step();
                continue;
            }
            hashes.size = size;
            hashes.mtime = mtime;
            if (!known) hashes.sha1 = SHA1::hash(bytes);
            if (builtinSized) hashes.sha256 = sha256Hex(bytes);
            rememberHashes(file, hashes);
        }
        if (!hashes.sha256.empty()) projectSha256.insert(hashes.sha256);
        if (!indexed) continue;

        const AudioAnalysis::Labels labels = AudioAnalysis::labelsOf(file);
        setLabels(entry, labels);
        AudioImportSettings settings;
        std::string settingsError;
        if (AudioImportSettings::load(file, settings, settingsError)) {
            entry.loopRegion = settings.loopStartMs >= 0.0 || settings.loopEndMs >= 0.0;
            entry.gainDb = settings.gainDb;
        }

        Json cached;
        std::string error;
        if (AudioAnalysis::readCache(sources.cacheDirectory, hashes.sha1, cached)) {
            setFeatures(entry, cached, labels);
        } else {
            if (bytes.empty() && !readBytes(file, bytes)) {
                entry.error = "can't read the file";
            } else if (cachedAnalysis(bytes, hashes.sha1, sources.cacheDirectory, cached, analysed, error)) {
                setFeatures(entry, cached, labels);
            } else {
                entry.error = error;
            }
        }
        clips.push_back(std::move(entry));
        step();
    }
    // project entries in path order, then the essentials by id, then the library's clips by id
    out = std::move(clips);
    // the project's Sound Events: their features are their first clip's, their length the longest's
    std::map<std::string, const Entry*> byId;
    for (const Entry& clip : out) byId[clip.id] = &clip;
    std::vector<Entry> events;
    for (const auto& [file, guid] : sources.events) {
        if (cancel && *cancel) return out;
        std::string text;
        SoundEventDef def;
        if (!readBytes(file, text) || !SoundEventIO::parse(text, def)) continue;
        Entry entry;
        entry.source = "project";
        entry.kind = "event";
        entry.file = file.lexically_normal();
        entry.path = relativeTo(file, sources.projectDirectory);
        const fs::path fromAssets = entry.file.lexically_relative(sources.assetsDirectory.lexically_normal());
        entry.id = fromAssets.generic_string();
        entry.guid = guid;
        entry.name = file.stem().string();
        entry.type = def.type;
        entry.inProject = true;
        std::vector<std::string> labels;
        AssetLabels::readFileLabels(file, false, labels);
        entry.tags = labels;
        for (const SoundEventClip& clip : def.clips) entry.clips.push_back(clip.clip.path);
        const auto first = entry.clips.empty() ? byId.end() : byId.find(entry.clips.front());
        if (entry.clips.empty()) {
            entry.error = "the event has no clip";
        } else if (first == byId.end() || !first->second->analysed) {
            entry.error = "its first clip " + entry.clips.front() + " is not a project audio file that reads";
        } else {
            const Entry& clip = *first->second;
            entry.analysed = true;
            entry.sha1 = clip.sha1;
            entry.soundKind = clip.soundKind;
            entry.lufs = clip.lufs;
            entry.truePeakDbtp = clip.truePeakDbtp;
            entry.centroidHz = clip.centroidHz;
            entry.flatness = clip.flatness;
            entry.attackMs = clip.attackMs;
            entry.pitchConfidence = clip.pitchConfidence;
            entry.bandShare = clip.bandShare;
            entry.loopRegion = clip.loopRegion;
            for (const std::string& name : entry.clips) {
                const auto found = byId.find(name);
                if (found != byId.end() && found->second->analysed) entry.durationMs = std::max(entry.durationMs, found->second->durationMs);
            }
        }
        events.push_back(std::move(entry));
    }
    out.insert(out.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });

    for (const SoundManifestClip& clip : SoundEssentials::all()) {
        const bool isReference = clip.id == sources.referenceEssential;
        if (!wantEssentials && !isReference) continue;
        if (cancel && *cancel) return out;
        Entry entry;
        entry.source = "essentials";
        entry.kind = "clip";
        entry.path = SoundEssentials::PREFIX + clip.id;
        entry.id = clip.id;
        entry.name = fs::path(clip.path).stem().string();
        entry.description = clip.description;
        const AudioAnalysis::Labels labels = SoundEssentials::labels(clip);
        setLabels(entry, labels);
        entry.inProject = projectSha256.count(clip.sha256) > 0;
        const std::string bytes = SoundEssentials::bytes(clip);
        Json cached;
        std::string error;
        if (cachedAnalysis(bytes, essentialSha1(clip), sources.cacheDirectory, cached, analysed, error)) {
            setFeatures(entry, cached, labels);
        } else {
            entry.error = error;
        }
        out.push_back(std::move(entry));
        step();
    }

    // -- the starter library's available clips: features from its analysis.json when
    // it is current (nothing analysed), else from the project's cache like any bytes
    for (const SoundManifestClip& clip : libraryClips) {
        const bool isReference = clip.id == sources.referenceLibrary;
        if (!wantLibrary && !isReference) continue;
        if (cancel && *cancel) return out;
        Entry entry;
        entry.source = "library";
        entry.kind = "clip";
        entry.path = SoundLibrary::PREFIX + clip.id;
        entry.id = clip.id;
        entry.name = fs::path(clip.path).stem().string();
        entry.description = clip.description;
        const AudioAnalysis::Labels labels = SoundLibrary::labels(clip);
        setLabels(entry, labels);
        if (clip.semantic) entry.heardTags = clip.semantic->heard;
        entry.inProject = projectSha256.count(clip.sha256) > 0;
        if (const Json* item = library.analysis(clip.id)) {
            setFeatures(entry, *item, labels);
        } else {
            std::string sha1, bytes, error;
            Json cached;
            if (knownLibrarySha1(clip.sha256, sha1) && AudioAnalysis::readCache(sources.cacheDirectory, sha1, cached)) {
                setFeatures(entry, cached, labels);
            } else if (!library.read(clip, bytes, error)) {
                entry.error = error;
            } else {
                sha1 = SHA1::hash(bytes);
                rememberLibrarySha1(clip.sha256, sha1);
                if (cachedAnalysis(bytes, sha1, sources.cacheDirectory, cached, analysed, error)) setFeatures(entry, cached, labels);
                else entry.error = error;
            }
        }
        out.push_back(std::move(entry));
        step();
    }
    return out;
}

std::vector<std::string> SoundSearch::eventProblems(const SoundEventDef& event, const std::vector<fs::path>& clipFiles,
                                                    const fs::path& cacheDirectory, int& analysed) {
    std::vector<std::string> out;
    struct Measured {
        bool ok = false;
        Entry entry;
        Json bandsDb;
        // the level as it plays: momentary max + its importer's gain + the clip's volume_db
        double level = NAN;
        double offsetDb = 0.0;
    };
    std::vector<Measured> clips(event.clips.size());
    for (size_t i = 0; i < event.clips.size() && i < clipFiles.size(); i++) {
        const fs::path& file = clipFiles[i];
        std::error_code ec;
        if (file.empty() || !fs::is_regular_file(file, ec)) continue;
        std::string bytes;
        if (!readBytes(file, bytes)) continue;
        Json cached;
        std::string error;
        const std::string sha1 = SHA1::hash(bytes);
        if (!cachedAnalysis(bytes, sha1, cacheDirectory, cached, analysed, error)) continue;
        Measured& m = clips[i];
        m.ok = true;
        m.entry.sha1 = sha1;
        setFeatures(m.entry, cached, AudioAnalysis::Labels());
        AudioImportSettings settings;
        std::string settingsError;
        if (AudioImportSettings::load(file, settings, settingsError)) m.entry.gainDb = settings.gainDb;
        m.bandsDb = cached.value("bands_db", Json::object());
        m.offsetDb = m.entry.gainDb + (double)event.clips[i].volumeDb;
        m.level = std::isfinite(m.entry.lufs) ? m.entry.lufs + m.offsetDb : NAN;
    }
    auto number = [](size_t i) { return std::to_string(i + 1); };
    char buffer[160];

    if (event.mode != SoundEventMode::Layer) {
        size_t quietest = SIZE_MAX, loudest = SIZE_MAX;
        for (size_t i = 0; i < clips.size(); i++) {
            if (!clips[i].ok || !std::isfinite(clips[i].level)) continue;
            if (quietest == SIZE_MAX || clips[i].level < clips[quietest].level) quietest = i;
            if (loudest == SIZE_MAX || clips[i].level > clips[loudest].level) loudest = i;
        }
        if (quietest != SIZE_MAX && clips[loudest].level - clips[quietest].level > EVENT_SPREAD_LU) {
            std::snprintf(buffer, sizeof(buffer), "loudness spread %.1f LU across clips (clip %s at %.1f, clip %s at %.1f LUFS-M as they play): ",
                          clips[loudest].level - clips[quietest].level, number(loudest).c_str(), clips[loudest].level,
                          number(quietest).c_str(), clips[quietest].level);
            out.push_back(std::string(buffer) + "the picks jump in level; use match_levels");
        }
        for (size_t i = 0; i < clips.size(); i++) {
            for (size_t j = i + 1; j < clips.size(); j++) {
                if (!clips[i].ok || !clips[j].ok) continue;
                if (event.clips[i].clip.path == event.clips[j].clip.path) continue;  // a weight trick, on purpose
                const bool same = clips[i].entry.sha1 == clips[j].entry.sha1;
                const double d = distance(clips[i].entry, clips[j].entry);
                const double levels = std::fabs(clips[i].level - clips[j].level);
                const bool sameLevel = !std::isfinite(levels) || levels < NEAR_DUPLICATE_LEVEL_LU;
                if (same || (std::isfinite(d) && d < SoundTypes::NEAR_DUPLICATE_DISTANCE && sameLevel)) {
                    out.push_back("clips " + number(i) + " and " + number(j) + " are near-duplicates" +
                                  (same ? std::string(" (the same bytes)") : std::string()) + ": they will sound like one clip");
                }
            }
        }
    } else if (clips.size() >= 2) {
        // per band, each layer's energy as it plays
        std::vector<std::array<double, AudioMeter::BANDS>> levels(clips.size());
        std::array<bool, AudioMeter::BANDS> audible = {};
        for (size_t i = 0; i < clips.size(); i++) {
            for (int b = 0; b < AudioMeter::BANDS; b++) {
                const double db = clips[i].ok ? numberOr(clips[i].bandsDb.value(AudioMeter::bandLabel(b), Json(nullptr)), AudioMeter::SILENCE_DB)
                                              : AudioMeter::SILENCE_DB;
                levels[i][b] = db + (clips[i].ok ? clips[i].offsetDb : 0.0);
                if (clips[i].ok && levels[i][b] > SoundTypes::SILENCE_THRESHOLD_DBFS) audible[b] = true;
            }
        }
        for (size_t i = 0; i < clips.size(); i++) {
            if (!clips[i].ok) continue;
            bool any = false, adds = false;
            for (int b = 0; b < AudioMeter::BANDS && !adds; b++) {
                if (!audible[b]) continue;
                any = true;
                double others = 0.0;
                for (size_t j = 0; j < clips.size(); j++) {
                    if (j != i && clips[j].ok) others += std::pow(10.0, levels[j][b] / 10.0);
                }
                const double othersDb = others > 0.0 ? 10.0 * std::log10(others) : AudioMeter::SILENCE_DB;
                if (levels[i][b] > othersDb - SoundTypes::LAYER_INAUDIBLE_DB) adds = true;
            }
            if (any && !adds) {
                std::snprintf(buffer, sizeof(buffer), "layer %s adds nothing measurable: %.0f dB or more under the others in every band",
                              number(i).c_str(), SoundTypes::LAYER_INAUDIBLE_DB);
                out.push_back(buffer);
            }
        }
    }
    return out;
}

std::string SoundSearch::brightnessOf(const Entry& entry) {
    if (!entry.analysed || !std::isfinite(entry.centroidHz)) return std::string();
    if (entry.centroidHz < DARK_CENTROID_HZ) return "dark";
    if (entry.centroidHz > BRIGHT_CENTROID_HZ) return "bright";
    return "neutral";
}

std::string SoundSearch::attackOf(const Entry& entry) {
    if (!entry.analysed) return std::string();
    if (entry.attackMs <= SHARP_ATTACK_MS) return "sharp";
    if (entry.attackMs > SOFT_ATTACK_MS) return "soft";
    return std::string();
}

bool SoundSearch::isTonal(const Entry& entry) {
    return entry.analysed && entry.pitchConfidence >= TONAL_CONFIDENCE;
}

bool SoundSearch::isLoop(const Entry& entry) {
    return entry.analysed && (entry.soundKind == SoundTypes::LOOP || entry.loopRegion);
}

SoundSearch::Entry SoundSearch::entryOfAnalysis(const Json& cached) {
    Entry entry;
    setFeatures(entry, cached, AudioAnalysis::Labels());
    return entry;
}

double SoundSearch::distance(const Entry& a, const Entry& b) {
    if (!a.analysed || !b.analysed || !std::isfinite(a.centroidHz) || !std::isfinite(b.centroidHz) || !std::isfinite(a.flatness) ||
        !std::isfinite(b.flatness)) {
        return NAN;
    }
    auto features = [](const Entry& e, double out[5]) {
        out[0] = std::log2(std::max(e.durationMs / 1000.0, 0.01)) / 4.0;
        out[1] = std::log2(std::max(e.attackMs, 0.5)) / 4.0;
        out[2] = std::log2(std::max(e.centroidHz, 1.0) / 1000.0) / 2.0;
        out[3] = e.flatness;
        out[4] = e.pitchConfidence;
    };
    double fa[5], fb[5];
    features(a, fa);
    features(b, fb);
    double squares = 0.0;
    for (int i = 0; i < 5; i++) squares += (fa[i] - fb[i]) * (fa[i] - fb[i]);
    double bands = 0.0;
    for (int band = 0; band < AudioMeter::BANDS; band++) bands += std::fabs(a.bandShare[band] - b.bandShare[band]);
    return std::sqrt(squares) + 0.5 * bands;
}

bool SoundSearch::search(const std::vector<Entry>& index, const Query& query, std::vector<Entry>& results, size_t& total,
                         std::string& error) {
    results.clear();
    total = 0;
    const std::vector<std::string> queryWords = words(query.text);
    std::vector<std::string> singulars;
    for (const std::string& word : queryWords) singulars.push_back(folded(word));
    const std::string type = lower(query.type);
    const Filters& f = query.filters;

    const Entry* reference = nullptr;
    if (!query.similarTo.empty()) {
        for (const Entry& entry : index) {
            if (entry.path == query.similarTo) reference = &entry;
        }
        if (!reference) {
            error = "similar_to " + query.similarTo + " is not a sound of the project's assets, the essentials or the starter library.";
            return false;
        }
        if (!std::isfinite(distance(*reference, *reference))) {
            error = "similar_to " + query.similarTo + " has no features to compare (" +
                    (reference->analysed ? std::string("it is silent") : reference->error) + ").";
            return false;
        }
    }

    struct Ranked {
        const Entry* entry;
        bool typeMatch;
        // words found as typed, not only as their singular: an exact match outranks a folded one
        int exactWords;
        int tagWords;
        // words found only among a library clip's heard tags (a model's guess): after its own
        // tags, before its name (decision 14)
        int heardWords;
        int nameWords;
        double distance;
    };
    std::vector<Ranked> ranked;
    for (const Entry& entry : index) {
        if (&entry == reference) continue;
        if (!inScopeOf(query.scope, entry)) continue;
        if (!type.empty() && lower(entry.type) != type) continue;
        if (!f.kind.empty() && entry.kind != f.kind) continue;
        if (f.any() && !entry.analysed) continue;
        if (f.minMs >= 0.0 && entry.durationMs < f.minMs) continue;
        if (f.maxMs >= 0.0 && entry.durationMs > f.maxMs) continue;
        if (!f.brightness.empty() && brightnessOf(entry) != f.brightness) continue;
        if (!f.attack.empty() && attackOf(entry) != f.attack) continue;
        if (f.tonal >= 0 && isTonal(entry) != (f.tonal == 1)) continue;
        if (f.loop >= 0 && isLoop(entry) != (f.loop == 1)) continue;

        bool all = true;
        int exactWords = 0, tagWords = 0, heardWords = 0, nameWords = 0;
        bool typeMatch = !type.empty();
        for (size_t w = 0; w < queryWords.size(); w++) {
            const std::string& word = queryWords[w];
            const std::string& singular = singulars[w];
            bool inTags = false, inHeard = false;
            for (const std::string& tag : entry.tags) inTags = inTags || matches(tag, word, singular);
            for (const std::string& tag : entry.heardTags) inHeard = inHeard || heardMatches(tag, word, singular);
            const bool inName = matches(entry.name, word, singular);
            if (lower(entry.type) == word || lower(entry.type) == singular) typeMatch = true;
            const bool found = inTags || inHeard || inName || matches(entry.path, word, singular) || matches(entry.type, word, singular) ||
                               matches(entry.description, word, singular);
            if (!found) {
                all = false;
                break;
            }
            bool exact = contains(entry.name, word) || contains(entry.path, word) || contains(entry.type, word) || contains(entry.description, word);
            for (const std::string& tag : entry.tags) exact = exact || contains(tag, word);
            for (const std::string& tag : entry.heardTags) exact = exact || heardExactly(tag, word);
            exactWords += exact ? 1 : 0;
            tagWords += inTags ? 1 : 0;
            heardWords += inHeard && !inTags ? 1 : 0;
            nameWords += inName ? 1 : 0;
        }
        if (!all) continue;
        double d = NAN;
        if (reference) {
            d = distance(entry, *reference);
            if (!std::isfinite(d)) continue;
        }
        ranked.push_back({&entry, typeMatch, exactWords, tagWords, heardWords, nameWords, d});
    }

    // project, essentials, then the starter library; then by path
    auto pathOrder = [](const Entry* a, const Entry* b) {
        if (a->source != b->source) return sourceRank(a->source) < sourceRank(b->source);
        return a->path < b->path;
    };
    if (reference) {
        std::stable_sort(ranked.begin(), ranked.end(), [&](const Ranked& a, const Ranked& b) {
            if (a.distance != b.distance) return a.distance < b.distance;
            return pathOrder(a.entry, b.entry);
        });
    } else {
        std::stable_sort(ranked.begin(), ranked.end(), [&](const Ranked& a, const Ranked& b) {
            if (a.typeMatch != b.typeMatch) return a.typeMatch;
            if (a.exactWords != b.exactWords) return a.exactWords > b.exactWords;
            if (a.tagWords != b.tagWords) return a.tagWords > b.tagWords;
            if (a.heardWords != b.heardWords) return a.heardWords > b.heardWords;
            if (a.nameWords != b.nameWords) return a.nameWords > b.nameWords;
            return pathOrder(a.entry, b.entry);
        });
    }
    total = ranked.size();
    const size_t limit = query.limit == EVERY_MATCH ? ranked.size() : (size_t)std::max(1, std::min(query.limit, MAX_LIMIT));
    for (size_t i = 0; i < ranked.size() && i < limit; i++) {
        Entry entry = *ranked[i].entry;
        entry.distance = ranked[i].distance;
        results.push_back(std::move(entry));
    }
    return true;
}

std::string SoundSearch::folded(const std::string& word) {
    auto ends = [&](const char* suffix) {
        const size_t n = std::strlen(suffix);
        return word.size() >= n && word.compare(word.size() - n, n, suffix) == 0;
    };
    const size_t n = word.size();
    if (n <= 3) return word;
    if (n > 4 && ends("ies")) return word.substr(0, n - 3) + "y";
    if (n > 4 && (ends("sses") || ends("shes") || ends("ches") || ends("xes") || ends("zzes"))) return word.substr(0, n - 2);
    if (word.back() == 's' && !ends("ss") && !ends("us") && !ends("is")) return word.substr(0, n - 1);
    return word;
}

SoundSearch::Hint SoundSearch::emptyHint(const std::vector<Entry>& index, const Query& query) {
    Hint hint;
    auto inScope = [&](const Entry& entry) {
        if (!inScopeOf(query.scope, entry)) return false;
        return query.filters.kind.empty() || entry.kind == query.filters.kind;
    };
    const std::vector<std::string> queryWords = words(query.text);
    std::vector<std::string> singulars;
    for (const std::string& word : queryWords) singulars.push_back(folded(word));
    const bool narrowed = !query.type.empty() || query.filters.any();
    const char* scopeText = query.scope == Scope::Project      ? " in the project"
                            : query.scope == Scope::Essentials ? " in the essentials"
                            : query.scope == Scope::Library    ? " in the starter library"
                                                               : "";

    if (queryWords.empty()) {
        if (narrowed) hint.text = std::string("No sound") + scopeText + " has this type and these filters: loosen them.";
        return hint;
    }

    // each word alone, in the scope (type and filters aside)
    std::vector<size_t> alone(queryWords.size(), 0);
    size_t all = 0;
    for (const Entry& entry : index) {
        if (!inScope(entry)) continue;
        bool every = true;
        for (size_t w = 0; w < queryWords.size(); w++) {
            if (entryHas(entry, queryWords[w], singulars[w])) alone[w]++;
            else every = false;
        }
        if (every) all++;
    }

    std::vector<size_t> unmatched;
    for (size_t w = 0; w < queryWords.size(); w++) {
        if (alone[w] == 0) unmatched.push_back(w);
    }
    if (!unmatched.empty()) {
        // the index's terms: the words of names, tags, types, folders, descriptions and (library
        // clips) heard tags, by how many sounds have them
        std::map<std::string, size_t> terms;
        for (const Entry& entry : index) {
            if (!inScope(entry)) continue;
            std::set<std::string> own;
            std::string text = entry.name + " " + entry.type + " " + entry.path + " " + entry.description;
            for (const std::string& tag : entry.tags) text += " " + tag;
            for (const std::string& tag : entry.heardTags) text += " " + tag;
            for (const std::string& word : tokens(text)) {
                if (word.size() < 3 || std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) continue;
                own.insert(folded(word));
            }
            for (const std::string& term : own) terms[term]++;
        }
        std::vector<std::vector<std::string>> nearest(queryWords.size());
        std::string missing;
        for (size_t u = 0; u < unmatched.size(); u++) {
            const size_t w = unmatched[u];
            const std::string& singular = singulars[w];
            const size_t limit = singular.size() <= 4 ? 1 : singular.size() <= 8 ? 2 : 3;
            struct Candidate {
                size_t distance;
                size_t count;
                std::string term;
            };
            std::vector<Candidate> candidates;
            for (const auto& [term, count] : terms) {
                const size_t d = editDistance(singular, term, limit);
                if (d <= limit) candidates.push_back({d, count, term});
            }
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                if (a.distance != b.distance) return a.distance < b.distance;
                if (a.count != b.count) return a.count > b.count;
                return a.term < b.term;
            });
            for (size_t c = 0; c < candidates.size() && c < MAX_SUGGESTIONS; c++) nearest[w].push_back(candidates[c].term);
            missing += std::string(u == 0 ? "" : u + 1 == unmatched.size() ? " or " : ", ") + "\"" + queryWords[w] + "\"";
        }
        hint.text = std::string("No sound") + scopeText + " has the word" + (unmatched.size() == 1 ? " " : "s ") + missing;
        // the queries: the first unmatched word's terms in turn, every other unmatched word its nearest
        const size_t first = unmatched[0];
        for (size_t c = 0; c < nearest[first].size(); c++) {
            std::string text;
            bool complete = true;
            for (size_t w = 0; w < queryWords.size(); w++) {
                std::string word = queryWords[w];
                if (alone[w] == 0) {
                    const std::vector<std::string>& options = nearest[w];
                    if (options.empty()) {
                        complete = false;
                        break;
                    }
                    word = w == first ? options[c] : options[0];
                }
                text += (text.empty() ? "" : " ") + word;
            }
            if (complete) hint.queries.push_back(text);
        }
        if (!hint.queries.empty()) {
            hint.text += ": did you mean";
            for (size_t q = 0; q < hint.queries.size(); q++) {
                hint.text += std::string(q == 0 ? " " : q + 1 == hint.queries.size() ? " or " : ", ") + "\"" + hint.queries[q] + "\"";
            }
            hint.text += "?";
        } else {
            hint.text += ", and no word of the sounds' names, tags or types is close to " + std::string(unmatched.size() == 1 ? "it" : "them") +
                         ": describe the sound differently, or search by type.";
        }
        return hint;
    }

    if (all > 0) {
        hint.text = std::to_string(all) + (all == 1 ? " sound" : " sounds") + scopeText + " match the words, but not " +
                    (query.type.empty() ? "these filters" : query.filters.any() ? "this type and these filters" : "this type") + ": loosen " +
                    (query.type.empty() || query.filters.any() ? "them." : "it.");
        return hint;
    }
    // every word matches alone, never all together: the words alone, the most found first
    std::vector<size_t> order(queryWords.size());
    for (size_t w = 0; w < order.size(); w++) order[w] = w;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return alone[a] > alone[b]; });
    hint.text = "Each word matches alone (";
    for (size_t i = 0; i < order.size(); i++) {
        hint.text += std::string(i == 0 ? "" : ", ") + "\"" + queryWords[order[i]] + "\" " + std::to_string(alone[order[i]]);
    }
    hint.text += std::string(") but no sound") + scopeText + " has them all: try fewer words.";
    for (size_t i = 0; i < order.size() && hint.queries.size() < MAX_SUGGESTIONS; i++) hint.queries.push_back(queryWords[order[i]]);
    return hint;
}

namespace {

Json roundedOrNull(double value, double step) {
    return std::isfinite(value) ? AudioAnalysis::rounded(value, step) : Json(nullptr);
}

} // namespace

Json SoundSearch::toJson(const Entry& entry, int n, const Project* project) {
    Json item = {{"n", n}};
    if (entry.source == "project") item["path"] = entry.path;
    else item["id"] = entry.path;
    item["source"] = entry.source;
    item["kind"] = entry.kind;
    if (entry.kind == "event") item["guid"] = entry.guid;
    item["type"] = entry.type.empty() ? Json(nullptr) : Json(entry.type);
    item["sound_kind"] = entry.soundKind.empty() ? Json(nullptr) : Json(entry.soundKind);
    item["labels"] = entry.tags;
    if (!entry.heardTags.empty()) item["heard_tags"] = entry.heardTags;
    item["duration_ms"] = entry.analysed ? AudioAnalysis::rounded(entry.durationMs, 0.1) : Json(nullptr);
    item["lufs"] = roundedOrNull(entry.analysed ? entry.lufs : NAN, 0.01);
    item["centroid_hz"] = roundedOrNull(entry.analysed ? entry.centroidHz : NAN, 1.0);
    if (entry.source == "library" && entry.licence.is_object()) {
        // the manifest's: who made it and where it came from (the url and attribution go into the
        // sidecar an import writes)
        item["licence"] = {{"id", entry.licence.value("id", std::string())}, {"author", entry.licence.value("author", std::string())},
                           {"source", entry.licence.value("source", std::string())}};
    } else {
        item["licence"] = entry.licence;
    }
    item["in_project"] = entry.inProject;
    if (entry.source != "project") item["import"] = "import_files sources:[" + entry.path + "]";
    if (std::isfinite(entry.distance)) item["distance"] = entry.distance;
    if (entry.kind == "event") {
        Json clips = Json::array();
        for (const std::string& clip : entry.clips) {
            if (project) {
                clips.push_back(project->resolveAssetPath(fs::u8path(clip)).lexically_normal().lexically_relative(
                    project->getProjectPath()).generic_string());
            } else {
                clips.push_back(clip);
            }
        }
        item["clips"] = clips;
    }
    if (!entry.analysed && !entry.error.empty()) item["error"] = entry.error;
    return item;
}

std::string SoundSearch::line(const Entry& entry, int n) {
    std::string text = std::to_string(n) + ". " + entry.path + " (" + entry.kind;
    if (entry.kind == "event") text += ", " + std::to_string(entry.clips.size()) + (entry.clips.size() == 1 ? " clip" : " clips");
    text += ")";
    if (!entry.type.empty()) text += " " + entry.type;
    if (entry.analysed) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), " %s, %.0f ms", entry.soundKind.c_str(), entry.durationMs);
        text += buffer;
        if (std::isfinite(entry.lufs)) {
            std::snprintf(buffer, sizeof(buffer), ", %.1f LUFS-M", entry.lufs);
            text += buffer;
        }
        const std::string bright = brightnessOf(entry);
        const std::string attack = attackOf(entry);
        if (!bright.empty()) text += ", " + bright;
        if (!attack.empty()) text += ", " + attack;
        if (isTonal(entry)) text += ", tonal";
        if (isLoop(entry)) text += ", loop";
    } else if (!entry.error.empty()) {
        text += " (not analysed: " + entry.error + ")";
    }
    if (!entry.tags.empty()) {
        text += "; labels";
        for (size_t i = 0; i < entry.tags.size(); i++) text += (i == 0 ? " " : ", ") + entry.tags[i];
    }
    if (!entry.heardTags.empty()) {
        text += "; heard (a guess)";
        for (size_t i = 0; i < entry.heardTags.size(); i++) text += (i == 0 ? " " : ", ") + entry.heardTags[i];
    }
    if (entry.source != "project" && entry.inProject) text += "; in the project";
    if (std::isfinite(entry.distance)) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "; distance %.3f", entry.distance);
        text += buffer;
    }
    return text;
}
