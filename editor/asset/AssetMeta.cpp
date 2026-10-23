// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetMeta.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <iterator>

using namespace jasperengine;

namespace {

    const char* const knownOrder[] = {"metaVersion", "guid", "formerGuids", "subAssets"};

    // A top-level key line: a key at column 0 (block-style YAML keeps the values of a
    // top-level key indented, and sequence items of a top-level key may start with "- ")
    bool topLevelKey(const std::string& line, std::string& key) {
        if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '#' || line[0] == '-' || line[0] == '%') {
            return false;
        }
        if (line.compare(0, 3, "---") == 0 || line.compare(0, 3, "...") == 0) {
            return false;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            return false;
        }
        if (colon + 1 < line.size() && line[colon + 1] != ' ' && line[colon + 1] != '\t') {
            return false; // "a:b" is a scalar, not a key
        }
        key = line.substr(0, colon);
        if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'') && key.back() == key.front()) {
            key = key.substr(1, key.size() - 2);
        }
        return true;
    }

    std::string quoteIfNeeded(const std::string& text) {
        if (text.empty()) {
            return "\"\"";
        }
        YAML::Emitter out;
        out << YAML::Flow << YAML::BeginSeq << text << YAML::EndSeq;
        std::string flow = out.c_str();          // "[text]" or "[\"te:xt\"]"
        return flow.substr(1, flow.size() - 2);
    }

    bool sameGuids(const std::vector<Guid>& a, const std::vector<Guid>& b) {
        return a == b;
    }

}

bool editor::AssetMeta::isKnown(const std::string& name) {
    return std::find(std::begin(knownOrder), std::end(knownOrder), name) != std::end(knownOrder);
}

bool editor::AssetMeta::parse(const std::string& input, AssetMeta& meta, std::string* error) {
    meta = AssetMeta();
    std::string text = input;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        text.erase(0, 3);
    }

    // Split into top-level sections; comment and blank lines before a key go with it
    std::vector<Section> sections;
    std::string pending;
    for (size_t begin = 0; begin < text.size();) {
        size_t end = text.find('\n', begin);
        std::string line = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        begin = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string key;
        if (topLevelKey(line, key)) {
            sections.push_back({key, pending + line + "\n"});
            pending.clear();
        } else if (sections.empty() || line.empty() || line[0] == '#') {
            pending += line + "\n";
        } else {
            sections.back().text += pending + line + "\n";
            pending.clear();
        }
    }
    if (!pending.empty()) {
        if (sections.empty()) {
            if (error) *error = "no keys";
            return false;
        }
        sections.back().text += pending;
    }

    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    if (!root.IsMap()) {
        if (error) *error = "not a map";
        return false;
    }
    try {
        meta.metaVersion = root["metaVersion"] ? root["metaVersion"].as<int>() : 0;
        meta.guid = root["guid"] ? Guid::parse(root["guid"].as<std::string>()) : Guid();
        if (meta.guid.isNull()) {
            if (error) *error = "no valid guid";
            return false;
        }
        if (const YAML::Node former = root["formerGuids"]) {
            for (const YAML::Node& item : former) {
                const Guid value = Guid::parse(item.as<std::string>());
                if (!value.isNull()) {
                    meta.formerGuids.push_back(value);
                }
            }
        }
        if (const YAML::Node subAssets = root["subAssets"]) {
            for (const YAML::Node& item : subAssets) {
                SubAsset entry;
                entry.key = item["key"] ? item["key"].as<std::string>() : std::string();
                entry.guid = item["guid"] ? Guid::parse(item["guid"].as<std::string>()) : Guid();
                entry.name = item["name"] ? item["name"].as<std::string>() : std::string();
                if (!entry.key.empty() && !entry.guid.isNull()) {
                    meta.subAssets.push_back(std::move(entry));
                }
            }
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    // Sorted as written; a hand edit out of order is put back in order on the next write
    std::vector<Guid> sortedFormer = meta.formerGuids;
    std::sort(sortedFormer.begin(), sortedFormer.end());
    sortedFormer.erase(std::unique(sortedFormer.begin(), sortedFormer.end()), sortedFormer.end());
    std::vector<SubAsset> sortedSub = meta.subAssets;
    std::stable_sort(sortedSub.begin(), sortedSub.end(), [](const SubAsset& a, const SubAsset& b) { return a.key < b.key; });

    meta.sections = std::move(sections);
    meta.parsedVersion = meta.metaVersion;
    meta.parsedGuid = meta.guid;
    meta.parsedFormer = meta.formerGuids;
    meta.parsedSubAssets = meta.subAssets;
    meta.formerGuids = std::move(sortedFormer);
    meta.subAssets = std::move(sortedSub);
    return true;
}

std::string editor::AssetMeta::canonical(const std::string& name, const AssetMeta& meta) {
    if (name == "metaVersion") {
        return "metaVersion: " + std::to_string(meta.metaVersion) + "\n";
    }
    if (name == "guid") {
        return "guid: " + meta.guid.str() + "\n";
    }
    if (name == "formerGuids") {
        if (meta.formerGuids.empty()) {
            return std::string();
        }
        std::string line = "formerGuids: [";
        for (size_t i = 0; i < meta.formerGuids.size(); i++) {
            line += (i ? ", " : "") + meta.formerGuids[i].str();
        }
        return line + "]\n";
    }
    if (name == "subAssets") {
        if (meta.subAssets.empty()) {
            return std::string();
        }
        std::string block = "subAssets:\n";
        for (const SubAsset& entry : meta.subAssets) {
            block += "  - {key: " + quoteIfNeeded(entry.key) + ", guid: " + entry.guid.str() +
                     ", name: " + quoteIfNeeded(entry.name) + "}\n";
        }
        return block;
    }
    return std::string();
}

std::string editor::AssetMeta::serialize() const {
    auto changed = [&](const std::string& name) {
        if (name == "metaVersion") return metaVersion != parsedVersion;
        if (name == "guid") return guid != parsedGuid;
        if (name == "formerGuids") return !sameGuids(formerGuids, parsedFormer);
        if (name == "subAssets") return !(subAssets == parsedSubAssets);
        return false;
    };

    // The file's sections in their order: a known one as written unless its value changed
    std::vector<Section> items;
    for (const Section& section : sections) {
        if (!isKnown(section.name)) {
            items.push_back(section);
        } else if (!changed(section.name)) {
            items.push_back(section);
        } else {
            const std::string text = canonical(section.name, *this);
            if (!text.empty()) {
                items.push_back({section.name, text});
            }
        }
    }
    // Known sections the file lacks go right after the known one before them in
    // knownOrder (the top for metaVersion)
    for (size_t k = 0; k < std::size(knownOrder); k++) {
        const std::string name = knownOrder[k];
        auto has = [&](const std::string& n) {
            return std::any_of(items.begin(), items.end(), [&](const Section& s) { return s.name == n; });
        };
        if (has(name) || std::any_of(sections.begin(), sections.end(), [&](const Section& s) { return s.name == name; })) {
            continue;
        }
        const std::string text = canonical(name, *this);
        if (text.empty()) {
            continue;
        }
        size_t at = 0;
        for (size_t previous = k; previous-- > 0;) {
            auto it = std::find_if(items.begin(), items.end(), [&](const Section& s) { return s.name == knownOrder[previous]; });
            if (it != items.end()) {
                at = static_cast<size_t>(it - items.begin()) + 1;
                break;
            }
        }
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(at), Section{name, text});
    }

    std::string out;
    for (const Section& item : items) {
        out += item.text;
    }
    return out;
}

bool editor::AssetMeta::hasSection(const std::string& name) const {
    return std::any_of(sections.begin(), sections.end(), [&](const Section& s) { return s.name == name; });
}

std::string editor::AssetMeta::getSection(const std::string& name) const {
    for (const Section& section : sections) {
        if (section.name != name) {
            continue;
        }
        // The text after "<name>:" on the key line, then the block below it
        std::string text = section.text;
        const size_t keyLine = text.find(name + ":");
        if (keyLine == std::string::npos) {
            return std::string();
        }
        std::string value = text.substr(keyLine + name.size() + 1);
        const size_t first = value.find_first_not_of(" \t");
        return first == std::string::npos ? std::string() : value.substr(first);
    }
    return std::string();
}

void editor::AssetMeta::setSection(const std::string& name, const std::string& valueYaml) {
    if (isKnown(name)) {
        return;
    }
    std::string text = name + ":";
    if (!valueYaml.empty()) {
        text += (valueYaml.front() == '\n' ? "" : " ") + valueYaml;
    }
    if (text.back() != '\n') {
        text += "\n";
    }
    for (Section& section : sections) {
        if (section.name == name) {
            section.text = text;
            return;
        }
    }
    sections.push_back({name, text});
}

void editor::AssetMeta::removeSection(const std::string& name) {
    if (isKnown(name)) {
        return;
    }
    sections.erase(std::remove_if(sections.begin(), sections.end(), [&](const Section& s) { return s.name == name; }), sections.end());
}

void editor::AssetMeta::addFormerGuid(const Guid& former) {
    if (former.isNull() || former == guid) {
        return;
    }
    auto it = std::lower_bound(formerGuids.begin(), formerGuids.end(), former);
    if (it == formerGuids.end() || *it != former) {
        formerGuids.insert(it, former);
    }
}

bool editor::AssetMeta::removeFormerGuid(const Guid& former) {
    auto it = std::lower_bound(formerGuids.begin(), formerGuids.end(), former);
    if (it == formerGuids.end() || *it != former) {
        return false;
    }
    formerGuids.erase(it);
    return true;
}

void editor::AssetMeta::setSubAssets(std::vector<SubAsset> entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const SubAsset& a, const SubAsset& b) { return a.key < b.key; });
    subAssets = std::move(entries);
}
