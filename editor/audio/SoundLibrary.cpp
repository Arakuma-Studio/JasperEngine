// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundLibrary.h"

#include "AudioProvenance.h"
#include "Out.h"
#include "util/FileUtils.h"
#include "util/SHA1.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

using Json = SoundLibrary::Json;

const char* const ANALYSIS_FORMAT = "jasperengine-sound-analysis";
// a licence or credits file is a few KB (Kenney's License.txt): anything larger is not one
const uint64_t MAX_LICENCE_BYTES = 64 * 1024;

std::string reinstall() {
    return std::string("reinstall the editor, or set ") + SoundLibrary::ENV_DIR;
}

// At most `limit` bytes of a regular file (not a symbolic link), into out; the file's size in size
bool readRegular(const fs::path& file, uint64_t limit, std::string& out, uint64_t& size, std::string& error) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(file, ec);
    if (ec || !fs::exists(st)) {
        error = "missing";
        return false;
    }
    if (fs::is_symlink(st)) {
        error = "a symbolic link, not a file";
        return false;
    }
    if (!fs::is_regular_file(st)) {
        error = "not a regular file";
        return false;
    }
    size = fs::file_size(file, ec);
    if (ec) {
        error = "can't read its size";
        return false;
    }
    if (size > limit) {
        error = "larger than " + std::to_string(limit) + " bytes";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "can't be read";
        return false;
    }
    out.assign((size_t)size, '\0');
    if (size > 0 && !in.read(&out[0], (std::streamsize)size)) {
        error = "can't be read";
        return false;
    }
    // a file that grew since: what was read is not the whole file
    if (in.peek() != std::char_traits<char>::eof()) {
        error = "changed while it was read";
        return false;
    }
    return true;
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
}

} // namespace

// ======================================================================== the opened library

struct SoundLibrary::Data {
    SoundLibraryStatus status;
    SoundManifest manifest;
    std::vector<SoundManifestClip> available;

    // analysis.json, parsed on first use; analysisState 0 before, 1 while, 2 after the parse
    mutable std::once_flag analysisOnce;
    mutable std::atomic<int> analysisState{0};
    mutable std::atomic<bool> analysisCurrent{false};
    mutable Json analysisItems = Json::object();
    // per id: the item's sha1 is the SHA-1 of the clip's bytes (1), it is not or the clip can't
    // be read (0)
    mutable std::mutex sha1Mutex;
    mutable std::map<std::string, bool> sha1Matches;
};

std::unique_ptr<SoundLibrary::Data> SoundLibrary::open(const fs::path& rootPath) {
    auto d = std::make_unique<Data>();
    std::error_code ec;
    const fs::path root = rootPath.empty() ? fs::path() : fs::absolute(rootPath, ec).lexically_normal();
    d->status.root = root;
    const std::string where = root.generic_string();
    if (root.empty() || !fs::is_directory(root, ec)) {
        d->status.error = "Starter library not found at " + (where.empty() ? std::string("(no directory)") : where) + " (" + reinstall() + ")";
        return d;
    }
    const fs::path manifestFile = root / MANIFEST;
    if (!fs::exists(fs::symlink_status(manifestFile, ec))) {
        d->status.error = "Starter library at " + where + " has no " + MANIFEST + " (" + reinstall() + ")";
        return d;
    }
    std::string text, error;
    uint64_t size = 0;
    if (!readRegular(manifestFile, MAX_MANIFEST_BYTES, text, size, error)) {
        d->status.error = "Starter library at " + where + ": " + MANIFEST + " is " + error + " (" + reinstall() + ")";
        return d;
    }
    if (!SoundManifest::parse(text, SoundManifest::LIBRARY_FORMAT, d->manifest, error)) {
        d->manifest = SoundManifest();
        d->status.error = "Starter library at " + where + " has an invalid " + MANIFEST + ": " + error + " (" + reinstall() + ")";
        return d;
    }
    d->status.available = true;
    d->status.version = d->manifest.version;
    d->status.clips = d->manifest.clips.size();
    // a regular file of the manifest's size, in a folder that is not a link, counts (no hashing:
    // a stat call per clip, the whole library in milliseconds)
    std::map<std::string, bool> folders;
    for (const SoundManifestClip& clip : d->manifest.clips) {
        d->status.bytes += clip.bytes;
        auto folder = folders.find(clip.group);
        if (folder == folders.end()) {
            const fs::file_status st = fs::symlink_status(root / clip.group, ec);
            folder = folders.emplace(clip.group, fs::is_directory(st) && !fs::is_symlink(st)).first;
        }
        const fs::path file = root / fs::path(clip.id + ".ogg");
        const fs::file_status st = fs::symlink_status(file, ec);
        const bool present = folder->second && fs::is_regular_file(st) && !fs::is_symlink(st) && fs::file_size(file, ec) == clip.bytes && !ec;
        if (present) d->available.push_back(clip);
        else d->status.missing++;
    }
    return d;
}

SoundLibrary& SoundLibrary::get() {
    static SoundLibrary library;
    return library;
}

const SoundLibrary::Data& SoundLibrary::data() const {
    if (const Data* d = current.load(std::memory_order_acquire)) return *d;
    std::lock_guard<std::mutex> lock(openMutex);
    if (const Data* d = current.load(std::memory_order_acquire)) return *d;
    opened.push_back(open(FileUtils::getSoundLibraryDir()));
    current.store(opened.back().get(), std::memory_order_release);
    return *opened.back();
}

void SoundLibrary::reopen(const fs::path& root) {
    std::lock_guard<std::mutex> lock(openMutex);
    opened.push_back(open(root));
    current.store(opened.back().get(), std::memory_order_release);
}

void SoundLibrary::setLogWarnings(bool on) {
    logWarnings = on;
}

namespace {

// the read warnings already logged (the root and the key), for warnOnce
struct Warned {
    std::mutex mutex;
    std::set<std::string> keys;
};

Warned& warned() {
    static Warned instance;
    return instance;
}

} // namespace

void SoundLibrary::warnOnce(const std::string& key, const std::string& message) const {
    Warned& w = warned();
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        if (!w.keys.insert(data().status.root.generic_string() + "\n" + key).second) return;
    }
    if (logWarnings) Out::warning(message);
}

// ======================================================================== queries

SoundLibraryStatus SoundLibrary::status() const {
    const Data& d = data();
    SoundLibraryStatus out = d.status;
    if (out.available) {
        // parses analysis.json on first use, but never waits for another thread's parse (warmUp's,
        // while the Browser draws its sources every frame)
        if (d.analysisState.load() == 0) analysis(std::string());
        out.analysisCurrent = d.analysisCurrent.load();
    }
    return out;
}

const SoundManifest& SoundLibrary::manifest() const {
    return data().manifest;
}

const std::vector<SoundManifestClip>& SoundLibrary::all() const {
    return data().available;
}

const SoundManifestClip* SoundLibrary::find(const std::string& id) const {
    const std::vector<SoundManifestClip>& clips = all();
    const auto it = std::lower_bound(clips.begin(), clips.end(), id, [](const SoundManifestClip& c, const std::string& key) { return c.id < key; });
    return it != clips.end() && it->id == id ? &*it : nullptr;
}

std::vector<std::string> SoundLibrary::types() const {
    std::set<std::string> unique;
    for (const SoundManifestClip& clip : all()) unique.insert(clip.group);
    return std::vector<std::string>(unique.begin(), unique.end());
}

bool SoundLibrary::isReference(const std::string& text) {
    return text.rfind(PREFIX, 0) == 0;
}

std::vector<const SoundManifestClip*> SoundLibrary::match(const std::string& ref, std::string& error) const {
    std::vector<const SoundManifestClip*> out;
    if (!isReference(ref)) {
        error = ref + " is not a library: reference";
        return out;
    }
    const Data& d = data();
    if (!d.status.available) {
        error = ref + ": " + d.status.error;
        return out;
    }
    const std::string pattern = ref.substr(std::string(PREFIX).size());
    auto unknown = [&](const std::string& what) {
        error = "unknown starter library " + what + ": use library:<type>/<name>, library:<type>/* or library:*; types: " + joined(types()) +
                " (list the ids with jasperengine-editor sound-library list)";
    };
    if (pattern == "*") {
        for (const SoundManifestClip& clip : d.available) out.push_back(&clip);
        if (out.empty()) error = "the starter library at " + d.status.root.generic_string() + " has no clips installed (" + reinstall() + ")";
        return out;
    }
    if (pattern.size() > 2 && pattern.compare(pattern.size() - 2, 2, "/*") == 0) {
        const std::string type = pattern.substr(0, pattern.size() - 2);
        for (const SoundManifestClip& clip : d.available) {
            if (clip.group == type) out.push_back(&clip);
        }
        if (!out.empty()) return out;
        const bool listed = std::any_of(d.manifest.clips.begin(), d.manifest.clips.end(), [&](const SoundManifestClip& c) { return c.group == type; });
        if (listed) {
            error = "every library:" + type + "/ clip is missing from the installed library at " + d.status.root.generic_string() + " (" + reinstall() + ")";
        } else {
            unknown("type \"" + type + "\"");
        }
        return out;
    }
    if (const SoundManifestClip* clip = find(pattern)) {
        out.push_back(clip);
        return out;
    }
    if (d.manifest.find(pattern)) {
        error = ref + " is missing from the installed library (" + absolutePath(*d.manifest.find(pattern)).generic_string() +
                " is not a regular file of the manifest's size): " + reinstall();
    } else {
        unknown("sound \"" + pattern + "\"");
    }
    return out;
}

fs::path SoundLibrary::absolutePath(const SoundManifestClip& clip) const {
    return data().status.root / fs::path(clip.id + ".ogg");
}

bool SoundLibrary::read(const SoundManifestClip& clip, std::string& bytes, std::string& error) const {
    bytes.clear();
    const Data& d = data();
    const std::string ref = PREFIX + clip.id;
    if (!d.status.available) {
        error = ref + ": " + d.status.error;
        return false;
    }
    // only a validated id of this manifest makes a path
    const SoundManifestClip* entry = SoundManifest::isValidId(clip.id) ? d.manifest.find(clip.id) : nullptr;
    if (!entry) {
        error = ref + " is not in the starter library's manifest";
        return false;
    }
    const fs::path file = absolutePath(*entry);
    std::error_code ec;
    const fs::file_status folder = fs::symlink_status(file.parent_path(), ec);
    if (fs::is_symlink(folder)) {
        error = ref + ": its folder " + file.parent_path().generic_string() + " is a symbolic link, which the library never follows (" + reinstall() + ")";
        return false;
    }
    uint64_t size = 0;
    std::string why;
    if (!readRegular(file, entry->bytes, bytes, size, why)) {
        bytes.clear();
        if (why.rfind("larger than", 0) == 0) {
            why = std::to_string(size) + " bytes, the manifest says " + std::to_string(entry->bytes);
            error = ref + " does not match its manifest (" + why + "): the installed library was modified; reinstall the editor";
            warnOnce(clip.id, error);
        } else {
            error = ref + " can't be read at " + file.generic_string() + " (" + why + "): " + reinstall();
        }
        return false;
    }
    if (size != entry->bytes) {
        bytes.clear();
        error = ref + " does not match its manifest (" + std::to_string(size) + " bytes, expected " + std::to_string(entry->bytes) +
                "): the installed library was modified; reinstall the editor";
        warnOnce(clip.id, error);
        return false;
    }
    const std::string sha = sha256Hex(bytes);
    if (sha != entry->sha256) {
        bytes.clear();
        error = ref + " does not match its manifest (SHA-256 " + sha + ", expected " + entry->sha256 +
                "): the installed library was modified; reinstall the editor";
        warnOnce(clip.id, error);
        return false;
    }
    return true;
}

bool SoundLibrary::read(const SoundManifestClip& clip, std::vector<uint8_t>& bytes, std::string& error) const {
    std::string text;
    const bool ok = read(clip, text, error);
    bytes.assign(text.begin(), text.end());
    return ok;
}

const Json* SoundLibrary::analysis(const std::string& id) const {
    const Data& d = data();
    if (!d.status.available) return nullptr;
    std::call_once(d.analysisOnce, [&]() {
        d.analysisState = 1;
        struct Parsed {
            const Data& d;
            ~Parsed() { d.analysisState = 2; }
        } parsed{d};
        const fs::path file = d.status.root / ANALYSIS;
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(file, ec))) return;
        std::string text, error;
        uint64_t size = 0;
        Json value;
        if (readRegular(file, MAX_ANALYSIS_BYTES, text, size, error)) {
            value = Json::parse(text, nullptr, false);
            if (value.is_discarded()) error = "not valid JSON";
            else if (!value.is_object() || value.value("format", std::string()) != ANALYSIS_FORMAT || value.value("version", 0) != 1 ||
                     !value.contains("items") || !value["items"].is_object()) {
                error = std::string("not a ") + ANALYSIS_FORMAT + " v1 file with items";
            }
        }
        if (!error.empty()) {
            warnOnce("\nanalysis", "Starter library: " + file.generic_string() + " is " + error +
                                       "; library searches analyse the clips instead (rewrite it with build_library.py analyse)");
            return;
        }
        // an older analyzer's numbers: stale, so searches analyse on demand
        if (value.value("analyzer_version", 0) != AudioAnalysis::VERSION) return;
        d.analysisItems = std::move(value["items"]);
        d.analysisCurrent = true;
    });
    if (!d.analysisCurrent || id.empty()) return nullptr;
    const auto it = d.analysisItems.find(id);
    if (it == d.analysisItems.end() || !it->is_object()) return nullptr;
    const SoundManifestClip* clip = find(id);
    if (!clip) return nullptr;
    {
        std::lock_guard<std::mutex> lock(d.sha1Mutex);
        const auto known = d.sha1Matches.find(id);
        if (known != d.sha1Matches.end()) return known->second ? &*it : nullptr;
    }
    // the item belongs to the clip's bytes (read once, through the hash-checked read)
    std::string bytes, error;
    const bool matches = read(*clip, bytes, error) && it->value("sha1", std::string()) == SHA1::hash(bytes);
    std::lock_guard<std::mutex> lock(d.sha1Mutex);
    d.sha1Matches[id] = matches;
    return matches ? &*it : nullptr;
}

// ======================================================================== verify

std::vector<SoundLibrary::VerifyIssue> SoundLibrary::verify() const {
    const Data& d = data();
    std::vector<VerifyIssue> issues;
    if (!d.status.available) {
        issues.push_back({d.status.root.generic_string(), d.status.error});
        return issues;
    }
    const fs::path& root = d.status.root;
    std::set<std::string> clipPaths;
    std::set<std::string> folders;
    for (const SoundManifestClip& clip : d.manifest.clips) {
        clipPaths.insert(clip.path);
        folders.insert(clip.group);
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(root / clip.group, ec))) {
            issues.push_back({clip.path, "its folder " + clip.group + " is a symbolic link, which the library never follows"});
            continue;
        }
        std::string bytes, why;
        uint64_t size = 0;
        if (!readRegular(root / fs::path(clip.path), clip.bytes, bytes, size, why)) {
            if (why.rfind("larger than", 0) == 0) why = std::to_string(size) + " bytes, the manifest says " + std::to_string(clip.bytes);
            issues.push_back({clip.path, why});
            continue;
        }
        if (size != clip.bytes) {
            issues.push_back({clip.path, std::to_string(size) + " bytes, the manifest says " + std::to_string(clip.bytes)});
            continue;
        }
        const std::string sha = sha256Hex(bytes);
        if (sha != clip.sha256) issues.push_back({clip.path, "SHA-256 " + sha + ", the manifest says " + clip.sha256});
    }

    // every other file: manifest.json, analysis.json and the licences are the only ones allowed
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::none, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(root);
        const std::string name = rel.generic_string();
        std::error_code sec;
        const fs::file_status st = it->symlink_status(sec);
        if (fs::is_symlink(st)) {
            if (!clipPaths.count(name) && !folders.count(name)) issues.push_back({name, "not in the manifest (a symbolic link)"});
            continue;
        }
        if (fs::is_directory(st)) {
            const bool known = name == LICENCES || folders.count(name);
            if (!known && fs::is_empty(it->path(), sec)) issues.push_back({name + "/", "not in the manifest (an empty folder)"});
            continue;
        }
        if (fs::is_regular_file(st)) total += it->file_size(sec);
        const bool licence = rel.parent_path() == fs::path(LICENCES) && fs::is_regular_file(st);
        if (clipPaths.count(name) || name == MANIFEST || name == ANALYSIS || licence) continue;
        issues.push_back({name, fs::is_regular_file(st) ? "not in the manifest" : "not in the manifest (not a regular file)"});
    }
    if (ec) issues.push_back({".", "the library folder can't be listed: " + ec.message()});

    for (const SoundManifestPack& pack : d.manifest.packs) {
        std::string text, why;
        uint64_t size = 0;
        if (!readRegular(root / fs::path(pack.licenceFile), MAX_LICENCE_BYTES, text, size, why)) {
            issues.push_back({pack.licenceFile, "pack " + pack.id + "'s licence file is " + why});
        } else if (text.find(LICENCE_MARKER) == std::string::npos || text.find(LICENCE_URL_MARKER) == std::string::npos) {
            issues.push_back({pack.licenceFile, std::string("pack ") + pack.id + "'s licence file lacks the CC0 markers (\"" + LICENCE_MARKER + "\", \"" +
                                                    LICENCE_URL_MARKER + "\")"});
        }
        if (!pack.creditsFile.empty() && !readRegular(root / fs::path(pack.creditsFile), MAX_LICENCE_BYTES, text, size, why)) {
            issues.push_back({pack.creditsFile, "pack " + pack.id + "'s credits file is " + why});
        }
    }
    if (total > MAX_TREE_BYTES) {
        issues.push_back({".", "the library is " + std::to_string(total) + " bytes, over its cap of " + std::to_string(MAX_TREE_BYTES) + " (36 MiB)"});
    }
    std::stable_sort(issues.begin(), issues.end(), [](const VerifyIssue& a, const VerifyIssue& b) { return a.path < b.path; });
    return issues;
}

// ======================================================================== labels

AudioAnalysis::Labels SoundLibrary::labels(const SoundManifestClip& clip) {
    AudioAnalysis::Labels labels;
    labels.type = clip.type;
    labels.manifestKind = clip.kind;
    labels.library = true;
    labels.tags = clip.tags;
    AudioProvenance provenance;
    provenance.values = clip.meta.value("provenance", nlohmann::json::object());
    provenance.values.erase("version");
    labels.licence = provenance.licenceJson();
    return labels;
}
