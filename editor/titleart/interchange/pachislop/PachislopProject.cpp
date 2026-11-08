// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/io/project.ts (parseProjectFile) by SafEight, used with the author's
// permission.

#include "PachislopProject.h"

#include "PachislopMigrate.h"

#include <cctype>
#include <regex>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;

bool pachislop::decodeBase64(const std::string& text, std::string& out, size_t limit) {
    out.clear();
    out.reserve(std::min(limit, text.size() / 4 * 3));
    uint32_t buffer = 0;
    int bits = 0;
    size_t padding = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') { padding++; continue; }
        else if (std::isspace((unsigned char)c)) continue;
        else return false;
        if (padding) return false;          // data after padding
        buffer = (buffer << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out.size() >= limit) return false;
            out.push_back((char)((buffer >> bits) & 0xFF));
        }
    }
    return padding <= 2;
}

bool pachislop::looksLikeProjectFile(const std::string& head) {
    size_t i = 0;
    if (head.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    while (i < head.size() && std::isspace((unsigned char)head[i])) i++;
    if (i >= head.size() || head[i] != '{') return false;
    static const std::regex format(R"re("format"\s*:\s*"pslot")re");
    return std::regex_search(head, format);
}

bool pachislop::parseProjectFile(const std::string& text, Bundle& out, std::string& code, std::string& error) {
    if (text.size() > MAX_FILE_BYTES) {
        code = "pachislop_too_large";
        error = "the project file is " + std::to_string(text.size() >> 20) + " MiB; Title Art imports Pachislop files of up to 256 MiB";
        return false;
    }
    const Json file = Json::parse(text, nullptr, false);
    if (file.is_discarded()) {
        code = "not_json";
        error = "Not a valid project file (JSON parse failed)";
        return false;
    }
    if (!file.is_object() || str(file, "format") != "pslot" || !file.contains("project") || !file["project"].is_object() ||
        !file["project"].contains("layers") || !file["project"]["layers"].is_array() || !file["project"].contains("comp") ||
        !file["project"]["comp"].is_object()) {
        code = "not_pachislop";
        error = "Not a Pachislop project file";
        return false;
    }
    const Json version = file.value("version", Json());
    const Json projectVersion = file["project"].value("version", Json());
    if (!isSupportedVersion(version) || projectVersion != version) {
        const bool newer = version.is_number() && version.get<double>() > PROJECT_VERSION;
        code = newer ? "pachislop_version_newer" : "pachislop_version_unsupported";
        error = newer ? "This is a version " + version.dump() + " Pachislop project, newer than the version " + std::to_string(PROJECT_VERSION) +
                            " this importer reads; Title Art imports versions 4 to " + std::to_string(PROJECT_VERSION)
                      : unsupportedVersionMessage(version);
        return false;
    }
    if (file["project"]["layers"].size() > MAX_LAYERS) {
        code = "pachislop_too_large";
        error = "the project has " + std::to_string(file["project"]["layers"].size()) + " layers; Title Art imports at most " +
                std::to_string(MAX_LAYERS);
        return false;
    }
    out = Bundle();
    out.version = (int)version.get<double>();
    std::string migrateError;
    if (!migrateProject(file["project"], out.project, migrateError)) {
        code = "not_pachislop";
        error = migrateError;
        return false;
    }
    const auto blobs = [&](const char* member, std::vector<Blob>& list) {
        auto it = file.find(member);
        if (it == file.end() || !it->is_array()) return;
        for (const Json& b : *it) {
            if (!b.is_object()) continue;
            Blob blob;
            blob.id = str(b, "id");
            blob.name = str(b, "name", blob.id);
            blob.mime = str(b, "mime", "application/octet-stream");
            const std::string data = str(b, "data");
            if (data.size() / 4 * 3 > MAX_ASSET_BYTES + 3) {
                out.notes.push_back("'" + blob.name + "' is larger than 64 MiB: left out");
                continue;
            }
            if (!decodeBase64(data, blob.data)) {
                out.notes.push_back("'" + blob.name + "' is not valid base64: left out");
                continue;
            }
            list.push_back(std::move(blob));
        }
    };
    blobs("fonts", out.fonts);
    blobs("assets", out.assets);
    if (out.version != PROJECT_VERSION) {
        out.notes.push_back("This project file was made with an earlier version (v" + std::to_string(out.version) +
                            ") and has been upgraded to v" + std::to_string(PROJECT_VERSION));
    }
    code.clear();
    return true;
}
