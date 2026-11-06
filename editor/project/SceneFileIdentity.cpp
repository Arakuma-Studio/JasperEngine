// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneFileIdentity.h"

#include "asset/AssetFileWrite.h"

#include "yaml-cpp/yaml.h"

#include <limits>
#include <vector>

using namespace jasperengine;

namespace {

    struct HeaderLine {
        size_t begin;
        size_t end;      // past the line's text, before its line break
        size_t next;     // the next line's begin
    };

    std::vector<HeaderLine> headerLines(const std::string& text) {
        std::vector<HeaderLine> lines;
        for (size_t begin = 0; begin < text.size();) {
            size_t brk = text.find('\n', begin);
            const size_t next = brk == std::string::npos ? text.size() : brk + 1;
            size_t end = brk == std::string::npos ? text.size() : brk;
            if (end > begin && text[end - 1] == '\r') end--;
            lines.push_back(HeaderLine{begin, end, next});
            begin = next;
        }
        return lines;
    }

    // A root (indent 0) "key:" line; the value's text after the colon in *value
    bool rootKeyLine(const std::string& text, const HeaderLine& line, const char* key, std::string* value) {
        const std::string prefix = std::string(key) + ":";
        if (line.end - line.begin < prefix.size() || text.compare(line.begin, prefix.size(), prefix) != 0) {
            return false;
        }
        if (value) *value = text.substr(line.begin + prefix.size(), line.end - line.begin - prefix.size());
        return true;
    }

    std::string headerScalarText(const std::string& value) {
        YAML::Emitter out;
        out << value;
        return out.c_str();
    }

}

uint32_t editor::sceneid::replacementId(const std::filesystem::path& relative, const std::function<bool(uint32_t)>& taken) {
    const std::string seed = relative.lexically_normal().generic_string();
    // FNV-1a: stable across runs and platforms
    uint32_t hash = 2166136261u;
    for (unsigned char c : seed) {
        hash ^= c;
        hash *= 16777619u;
    }
    uint32_t id = hash == 0 ? 1u : hash;
    for (uint64_t attempt = 0; attempt < std::numeric_limits<uint32_t>::max() && taken && taken(id); attempt++) {
        id = id == std::numeric_limits<uint32_t>::max() ? 1u : id + 1u;
    }
    return id;
}

bool editor::sceneid::readSceneHeader(const std::string& text, uint32_t& id, std::string& name) {
    bool foundId = false;
    for (const HeaderLine& line : headerLines(text)) {
        std::string value;
        try {
            if (!foundId && rootKeyLine(text, line, "id", &value)) {
                id = YAML::Load(value).as<uint32_t>();
                foundId = true;
            } else if (rootKeyLine(text, line, "name", &value)) {
                name = YAML::Load(value).as<std::string>();
            }
        } catch (const YAML::Exception&) {
            return false;
        }
    }
    return foundId;
}

bool editor::sceneid::rewriteSceneHeaderText(std::string& text, std::optional<uint32_t> id, const std::optional<std::string>& name) {
    bool idPlaced = !id.has_value();
    bool namePlaced = !name.has_value();
    std::string updated;
    updated.reserve(text.size() + 32);
    for (const HeaderLine& line : headerLines(text)) {
        if (!idPlaced && rootKeyLine(text, line, "id", nullptr)) {
            updated += "id: " + std::to_string(*id);
            updated.append(text, line.end, line.next - line.end);
            idPlaced = true;
            continue;
        }
        if (!namePlaced && rootKeyLine(text, line, "name", nullptr)) {
            updated += "name: " + headerScalarText(*name);
            updated.append(text, line.end, line.next - line.end);
            namePlaced = true;
            continue;
        }
        updated.append(text, line.begin, line.next - line.begin);
    }
    if (!idPlaced || !namePlaced) {
        return false;
    }
    text = std::move(updated);
    return true;
}

bool editor::sceneid::rewriteSceneHeader(const std::filesystem::path& file, std::optional<uint32_t> id, const std::optional<std::string>& name,
                                         std::string* error) {
    const assetio::ReadStamp read = assetio::stampOf(file);
    std::string text;
    if (!read.exists || !assetio::readFile(file, text)) {
        if (error) *error = "cannot read " + file.generic_string();
        return false;
    }
    if (!rewriteSceneHeaderText(text, id, name)) {
        if (error) *error = file.generic_string() + " has no scene id or name at its root";
        return false;
    }
    bool superseded = false;
    if (!assetio::rewriteFile(file, text, assetio::Durability::Document, read, &superseded, error)) {
        if (superseded && error) *error = file.generic_string() + " changed while its scene id was written";
        return false;
    }
    return true;
}
