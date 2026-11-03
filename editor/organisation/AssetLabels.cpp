// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetLabels.h"

#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetMeta.h"
#include "asset/AssetTypes.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    struct Line {
        size_t begin = 0;
        size_t end = 0;   // past its '\n'
    };

    std::vector<Line> splitLines(const std::string& text) {
        std::vector<Line> lines;
        for (size_t begin = 0; begin < text.size();) {
            size_t end = text.find('\n', begin);
            end = end == std::string::npos ? text.size() : end + 1;
            lines.push_back({begin, end});
            begin = end;
        }
        return lines;
    }

    std::string lineText(const std::string& text, const Line& line) {
        std::string out = text.substr(line.begin, line.end - line.begin);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }

    // `key:` at column 0
    bool isTopKey(const std::string& line, const char* key) {
        const size_t length = std::char_traits<char>::length(key);
        return line.compare(0, length, key) == 0 && line.size() > length && line[length] == ':' &&
               (line.size() == length + 1 || line[length + 1] == ' ' || line[length + 1] == '\t');
    }

    // a sequence item or a continuation of a block value (indented)
    bool isBlockLine(const std::string& line) {
        return !line.empty() && (line[0] == ' ' || line[0] == '\t' || line[0] == '-');
    }

}

// ---- labels as text

bool AssetLabels::normalizeLabel(const std::string& raw, std::string& label, std::string* why) {
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) begin++;
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) end--;
    label = raw.substr(begin, end - begin);
    if (label.empty() || label.size() > MAX_LENGTH) {
        if (why) *why = "A label has 1 to 32 characters.";
        return false;
    }
    for (unsigned char c : label) {
        if (!(std::isalnum(c) || c == ' ' || c == '_' || c == '.' || c == '-')) {
            if (why) *why = "A label uses letters, digits, spaces and _ . - only.";
            return false;
        }
    }
    return true;
}

bool AssetLabels::sameLabel(const std::string& a, const std::string& b) {
    return lower(a) == lower(b);
}

std::vector<std::string> AssetLabels::sortedUnique(std::vector<std::string> labels) {
    std::stable_sort(labels.begin(), labels.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    labels.erase(std::unique(labels.begin(), labels.end(), [](const std::string& a, const std::string& b) { return sameLabel(a, b); }),
                 labels.end());
    return labels;
}

std::string AssetLabels::formatList(const std::vector<std::string>& labels) {
    YAML::Emitter out;
    out << YAML::Flow << YAML::BeginSeq;
    for (const std::string& label : labels) out << label;
    out << YAML::EndSeq;
    return out.c_str();
}

std::vector<std::string> AssetLabels::parseList(const std::string& yamlValue) {
    std::vector<std::string> labels;
    try {
        const YAML::Node node = YAML::Load(yamlValue);
        if (node.IsSequence()) {
            for (const auto& item : node) {
                std::string label;
                if (item.IsScalar() && normalizeLabel(item.as<std::string>(""), label)) labels.push_back(label);
            }
        } else if (node.IsScalar()) {
            std::string label;
            if (normalizeLabel(node.as<std::string>(""), label)) labels.push_back(label);
        }
    } catch (const std::exception&) {
        labels.clear();
    }
    return sortedUnique(std::move(labels));
}

std::vector<std::string> AssetLabels::readInFileLabels(const std::string& text) {
    const std::vector<Line> lines = splitLines(text);
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string line = lineText(text, lines[i]);
        if (!isTopKey(line, "labels")) continue;
        std::string value = line.substr(7);
        bool blank = value.find_first_not_of(" \t") == std::string::npos;
        if (blank) {
            for (size_t j = i + 1; j < lines.size(); j++) {
                const std::string item = lineText(text, lines[j]);
                if (!isBlockLine(item)) break;
                value += "\n" + item;
            }
        }
        return parseList(value);
    }
    return {};
}

std::string AssetLabels::withInFileLabels(const std::string& text, const std::vector<std::string>& labels) {
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::vector<Line> lines = splitLines(text);
    std::string out;
    out.reserve(text.size() + 64);
    // the labels go right after the identity block (guid:, formerGuids: and its items)
    size_t insertAfter = std::string::npos;
    bool inIdentity = false;
    bool skippingLabels = false;
    std::vector<std::string> kept;
    for (const Line& line : lines) {
        const std::string current = lineText(text, line);
        if (skippingLabels) {
            if (isBlockLine(current)) continue;
            skippingLabels = false;
        }
        if (isTopKey(current, "labels")) {
            skippingLabels = true;
            continue;
        }
        if (isTopKey(current, "guid")) {
            inIdentity = true;
            kept.push_back(text.substr(line.begin, line.end - line.begin));
            insertAfter = kept.size();
            continue;
        }
        if (inIdentity && (isTopKey(current, "formerGuids") || (isBlockLine(current) && insertAfter == kept.size()))) {
            kept.push_back(text.substr(line.begin, line.end - line.begin));
            insertAfter = kept.size();
            continue;
        }
        inIdentity = false;
        kept.push_back(text.substr(line.begin, line.end - line.begin));
    }
    if (labels.empty()) {
        for (const std::string& line : kept) out += line;
        return out;
    }
    std::string labelLine = "labels: " + formatList(labels) + newline;
    if (insertAfter == std::string::npos) {
        // no identity: before the document's first key (past comments, directives, ---)
        insertAfter = 0;
        for (size_t i = 0; i < kept.size(); i++) {
            std::string line = kept[i];
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#' || line[0] == '%' || line.compare(0, 3, "---") == 0) {
                insertAfter = i + 1;
                continue;
            }
            break;
        }
    }
    for (size_t i = 0; i < kept.size(); i++) {
        if (i == insertAfter) out += labelLine;
        out += kept[i];
    }
    if (insertAfter >= kept.size()) {
        if (!out.empty() && out.back() != '\n') out += newline;
        out += labelLine;
    }
    return out;
}

bool AssetLabels::readFileLabels(const fs::path& absoluteAsset, bool inFile, std::vector<std::string>& labels) {
    labels.clear();
    std::string text;
    if (inFile) {
        if (!assetio::readFile(absoluteAsset, text)) return false;
        labels = readInFileLabels(text);
        return true;
    }
    if (!assetio::readFile(AssetTypes::metaPathFor(absoluteAsset), text)) return false;
    AssetMeta meta;
    if (!AssetMeta::parse(text, meta)) return false;
    if (meta.hasSection("labels")) labels = parseList(meta.getSection("labels"));
    return true;
}

bool AssetLabels::writeFileLabels(const fs::path& absoluteAsset, bool inFile, const std::vector<std::string>& labels,
                                  std::string* error, bool* written) {
    if (written) *written = false;
    std::string text;
    const fs::path file = inFile ? absoluteAsset : AssetTypes::metaPathFor(absoluteAsset);
    if (!assetio::readFile(file, text)) {
        if (error) *error = "Could not read " + file.generic_string();
        return false;
    }
    std::string updated;
    if (inFile) {
        updated = withInFileLabels(text, labels);
    } else {
        AssetMeta meta;
        std::string why;
        if (!AssetMeta::parse(text, meta, &why)) {
            if (error) *error = file.generic_string() + " is not a meta file (" + why + ")";
            return false;
        }
        if (labels.empty()) meta.removeSection("labels");
        else meta.setSection("labels", formatList(labels));
        updated = meta.serialize();
    }
    return assetio::writeFile(file, updated, assetio::Durability::Document, error, written);
}

// ---- colours

const std::vector<LabelColor>& AssetLabels::defaultPalette() {
    // OpenEngine's nine tag colours, maximally separated in hue (Source/UI/EditorTags.cpp)
    static const std::vector<LabelColor> palette = {
        {"Red", "#FF5252"}, {"Orange", "#FF7043"}, {"Yellow", "#FFEE58"}, {"Green", "#66BB6A"}, {"Teal", "#26A69A"},
        {"Blue", "#42A5F5"}, {"Indigo", "#7986CB"}, {"Purple", "#BA68C8"}, {"Pink", "#F06292"},
    };
    return palette;
}

std::string AssetLabels::paletteColorFor(const std::string& label) {
    // FNV-1a of the lower-case name: the same colour on every machine and run
    uint32_t hash = 2166136261u;
    for (char c : lower(label)) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return defaultPalette()[hash % defaultPalette().size()].color;
}

bool AssetLabels::isColor(const std::string& text) {
    if (text.size() != 7 || text[0] != '#') return false;
    return std::all_of(text.begin() + 1, text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// ---- the store

AssetLabels::~AssetLabels() {
    close();
}

bool AssetLabels::assetFile(const Guid& guid, fs::path& absolute, bool& inFile, std::string& relative) const {
    if (!project) return false;
    AssetDatabase::AssetInfo info;
    if (!project->getAssetDatabase().getInfo(guid, info)) return false;
    const AssetTypeInfo& typeInfo = AssetTypes::info(info.type);
    inFile = typeInfo.identity == AssetIdentity::InFile;
    relative = info.path;
    absolute = project->getProjectPath() / info.path;
    return true;
}

void AssetLabels::open(Project* owner) {
    // the colours come from project.yaml (setEntries) and stay; the labels are read again
    if (loadingStarted) {
        loading.cancel();
        loading.wait();
        loadingStarted = false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        byGuid.clear();
        touchedWhileLoading.clear();
        ready = false;
        revision++;
    }
    project = owner;
    // the files to read, listed now; read on a job
    struct Source {
        Guid guid;
        fs::path absolute;
        bool inFile = false;
    };
    auto sources = std::make_shared<std::vector<Source>>();
    project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (info.type == AssetType::SubAsset || info.type == AssetType::Unknown) return;
        sources->push_back({info.guid, project->getProjectPath() / info.path,
                            AssetTypes::info(info.type).identity == AssetIdentity::InFile});
    });
    loading = jobs::submit([sources]() {
        auto pending = std::make_shared<Pending>();
        for (const Source& source : *sources) {
            if (jobs::cancelRequested()) break;
            std::vector<std::string> labels;
            if (readFileLabels(source.absolute, source.inFile, labels) && !labels.empty()) {
                pending->labels[source.guid] = std::move(labels);
            }
        }
        return pending;
    }, {jobs::Lane::Background, jobs::GateClass::Default, "asset-labels"});
    loadingStarted = true;
}

void AssetLabels::close() {
    if (loadingStarted) {
        loading.cancel();
        loading.wait();
        loadingStarted = false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    byGuid.clear();
    touchedWhileLoading.clear();
    palette.clear();
    ready = false;
    project = nullptr;
    revision++;
}

void AssetLabels::update() {
    if (!loadingStarted || !loading.isReady()) return;
    loadingStarted = false;
    std::shared_ptr<Pending> pending;
    try {
        pending = loading.get();
    } catch (const std::exception&) {
        pending = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (pending) {
        // edits made while it read win over what it read
        for (auto& [guid, labels] : pending->labels) {
            if (!touchedWhileLoading.count(guid)) byGuid.emplace(guid, std::move(labels));
        }
    }
    ready = true;
    revision++;
}

bool AssetLabels::isReady() const {
    std::lock_guard<std::mutex> lock(mutex);
    return ready;
}

void AssetLabels::waitUntilReady() {
    if (loadingStarted) loading.wait();
    update();
}

uint64_t AssetLabels::version() const {
    std::lock_guard<std::mutex> lock(mutex);
    return revision;
}

std::vector<std::string> AssetLabels::labelsOf(const Guid& guid) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = byGuid.find(guid);
    return it == byGuid.end() ? std::vector<std::string>() : it->second;
}

std::vector<std::pair<Guid, std::vector<std::string>>> AssetLabels::all() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::pair<Guid, std::vector<std::string>>> out(byGuid.begin(), byGuid.end());
    return out;
}

std::vector<AssetLabels::LabelInfo> AssetLabels::labels() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<std::string, LabelInfo> byName;
    for (const LabelColor& entry : palette) {
        LabelInfo& info = byName[lower(entry.name)];
        info.name = entry.name;
        info.color = entry.color;
        info.hasEntry = true;
    }
    for (const auto& [guid, labels] : byGuid) {
        for (const std::string& label : labels) {
            LabelInfo& info = byName[lower(label)];
            if (info.name.empty()) info.name = label;
            info.assets++;
        }
    }
    std::vector<LabelInfo> out;
    for (auto& [key, info] : byName) {
        if (info.color.empty()) info.color = paletteColorFor(info.name);
        out.push_back(std::move(info));
    }
    return out;
}

std::string AssetLabels::colorOf(const std::string& label) const {
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const LabelColor& entry : palette) {
            if (sameLabel(entry.name, label) && isColor(entry.color)) return entry.color;
        }
    }
    return paletteColorFor(label);
}

std::string AssetLabels::canonical(const std::string& label) const {
    std::lock_guard<std::mutex> lock(mutex);
    for (const LabelColor& entry : palette) {
        if (sameLabel(entry.name, label)) return entry.name;
    }
    for (const auto& [guid, labels] : byGuid) {
        for (const std::string& existing : labels) {
            if (sameLabel(existing, label)) return existing;
        }
    }
    return label;
}

AssetLabels::WriteResult AssetLabels::setLabels(const std::vector<std::pair<Guid, std::vector<std::string>>>& changes) {
    WriteResult result;
    struct Done {
        Guid guid;
        fs::path absolute;
        bool inFile = false;
        std::vector<std::string> before;
    };
    std::vector<Done> done;
    auto rollback = [&done]() {
        for (auto it = done.rbegin(); it != done.rend(); ++it) writeFileLabels(it->absolute, it->inFile, it->before);
    };
    std::vector<std::pair<Guid, std::vector<std::string>>> applied;
    for (const auto& [guid, requested] : changes) {
        fs::path absolute;
        bool inFile = false;
        std::string relative;
        if (!assetFile(guid, absolute, inFile, relative)) {
            rollback();
            result.error = "No asset with GUID " + guid.str();
            result.filesWritten.clear();
            return result;
        }
        std::vector<std::string> labels;
        for (const std::string& raw : requested) {
            std::string label;
            std::string why;
            if (!normalizeLabel(raw, label, &why)) {
                rollback();
                result.error = "\"" + raw + "\": " + why;
                result.filesWritten.clear();
                return result;
            }
            labels.push_back(canonical(label));
        }
        labels = sortedUnique(std::move(labels));
        if (labels.size() > MAX_LABELS) {
            rollback();
            result.error = relative + ": an asset has at most 32 labels";
            result.filesWritten.clear();
            return result;
        }
        std::vector<std::string> before = labelsOf(guid);
        if (before == labels) continue;
        std::string error;
        bool written = false;
        if (!writeFileLabels(absolute, inFile, labels, &error, &written)) {
            rollback();
            result.error = error;
            result.filesWritten.clear();
            return result;
        }
        done.push_back({guid, absolute, inFile, before});
        applied.push_back({guid, labels});
        if (written) result.filesWritten.push_back(inFile ? relative : relative + AssetTypes::metaExtension);
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [guid, labels] : applied) {
            if (!ready) touchedWhileLoading.insert(guid);
            if (labels.empty()) byGuid.erase(guid);
            else byGuid[guid] = std::move(labels);
        }
        revision++;
    }
    result.ok = true;
    return result;
}

void AssetLabels::refreshAsset(const Guid& guid) {
    fs::path absolute;
    bool inFile = false;
    std::string relative;
    std::vector<std::string> labels;
    const bool known = assetFile(guid, absolute, inFile, relative);
    if (known) readFileLabels(absolute, inFile, labels);
    std::lock_guard<std::mutex> lock(mutex);
    if (!ready) touchedWhileLoading.insert(guid);
    auto it = byGuid.find(guid);
    const bool had = it != byGuid.end();
    if (labels.empty()) {
        if (had) {
            byGuid.erase(it);
            revision++;
        }
    } else if (!had || it->second != labels) {
        byGuid[guid] = std::move(labels);
        revision++;
    }
}

void AssetLabels::setEntries(std::vector<LabelColor> entries) {
    std::lock_guard<std::mutex> lock(mutex);
    palette.clear();
    for (LabelColor& entry : entries) {
        std::string name;
        if (!normalizeLabel(entry.name, name)) continue;
        if (std::any_of(palette.begin(), palette.end(), [&](const LabelColor& other) { return sameLabel(other.name, name); })) continue;
        palette.push_back({name, isColor(entry.color) ? entry.color : paletteColorFor(name)});
    }
    std::sort(palette.begin(), palette.end(), [](const LabelColor& a, const LabelColor& b) { return lower(a.name) < lower(b.name); });
    revision++;
}
