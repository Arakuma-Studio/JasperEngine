// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ContentManifest.h"

#include "serialization/CookedScene.h"

using namespace jasperengine;
using namespace jasperengine::player;
using nlohmann::json;

json ContentManifest::toJson() const {
    json out;
    out["schemaVersion"] = SCHEMA_VERSION;
    out["engineVersion"] = engineVersion;
    out["contentFormatVersion"] = contentFormatVersion;
    out["playerId"] = playerId;
    out["playerSha256"] = playerSha256;
    out["method"] = method;
    out["backend"] = backend;
    out["startScene"] = startScene;
    out["scenes"] = scenes;
    out["prefabs"] = prefabs;
    out["shaders"] = shaders;
    json fileMap = json::object();
    for (const auto& [path, file] : files) fileMap[path] = {{"sha256", file.sha256}, {"size", file.size}};
    out["files"] = fileMap;
    out["packs"] = json::array();
    for (const ManifestPack& pack : packs) out["packs"].push_back({{"file", pack.file}, {"sha256", pack.sha256}, {"entries", pack.entries}});
    out["notices"] = notices;
    out["allowPlayerMismatch"] = allowPlayerMismatch;
    return out;
}

bool ContentManifest::fromJson(const json& in, ContentManifest& out, std::string& error) {
    try {
        const int schema = in.value("schemaVersion", 0);
        if (schema < 1) {
            error = "content-manifest.json has no schemaVersion";
            return false;
        }
        if (schema > SCHEMA_VERSION) {
            error = "content-manifest.json is schema " + std::to_string(schema) + ", this player reads " +
                    std::to_string(SCHEMA_VERSION) + " (build the game with the editor of this player's version)";
            return false;
        }
        ContentManifest manifest;
        manifest.engineVersion = in.value("engineVersion", "");
        manifest.contentFormatVersion = in.value("contentFormatVersion", "");
        manifest.playerId = in.value("playerId", "");
        manifest.playerSha256 = in.value("playerSha256", "");
        manifest.method = in.value("method", "");
        manifest.backend = in.value("backend", "");
        manifest.startScene = in.value("startScene", 0u);
        manifest.scenes = in.value("scenes", std::vector<std::string>{});
        manifest.prefabs = in.value("prefabs", std::vector<std::string>{});
        manifest.shaders = in.value("shaders", std::vector<std::string>{});
        // (a named value: items() of a temporary dangles in a range-for before C++23)
        const json fileMap = in.value("files", json::object());
        for (const auto& [path, file] : fileMap.items())
            manifest.files[path] = {file.value("sha256", ""), file.value("size", uint64_t(0))};
        for (const json& pack : in.value("packs", json::array()))
            manifest.packs.push_back({pack.value("file", ""), pack.value("sha256", ""), pack.value("entries", uint64_t(0))});
        manifest.notices = in.value("notices", std::vector<std::string>{});
        manifest.allowPlayerMismatch = in.value("allowPlayerMismatch", false);
        out = std::move(manifest);
        return true;
    } catch (const json::exception& e) {
        error = std::string("content-manifest.json: ") + e.what();
        return false;
    }
}

bool ContentManifest::parse(const std::string& text, ContentManifest& out, std::string& error) {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        error = "content-manifest.json is not valid JSON";
        return false;
    }
    return fromJson(parsed, out, error);
}

bool player::checkContent(const ContentManifest& manifest, const std::string& playerId, const ContentAccess& access, bool verifyHashes,
                          std::vector<std::string>& problems) {
    const std::string major = manifest.contentFormatVersion.substr(0, manifest.contentFormatVersion.find('.'));
    if (major != std::to_string(cook::FORMAT_MAJOR)) {
        problems.push_back("content format " + manifest.contentFormatVersion + ", this player reads " + cook::contentFormatVersion() +
                           ": build the game with the editor of this player's version");
    }
    if (!playerId.empty() && manifest.playerId != playerId && !manifest.allowPlayerMismatch) {
        problems.push_back("the content was built for player " + manifest.playerId + ", this is " + playerId +
                           ": build it again for this player");
    }
    const std::string startFile = "scenes/" + std::to_string(manifest.startScene) + ".jscn";
    if (manifest.method == "prebuilt" && !access.exists(startFile)) problems.push_back("the start scene " + startFile + " is missing");
    for (const std::string& scene : manifest.scenes)
        if (!access.exists(scene)) problems.push_back("scene " + scene + " is missing");
    for (const std::string& prefab : manifest.prefabs)
        if (!access.exists(prefab)) problems.push_back("prefab " + prefab + " is missing");
    for (const std::string& shader : manifest.shaders)
        if (!access.exists("assets/shaders/" + shader)) problems.push_back("shader " + shader + " is missing");
    for (const ManifestPack& pack : manifest.packs)
        if (!access.exists(pack.file)) problems.push_back("content pack " + pack.file + " is missing");
    if (verifyHashes) {
        for (const auto& [path, file] : manifest.files) {
            std::string hex;
            uint64_t size = 0;
            if (!access.sha256(path, hex, size)) {
                problems.push_back(path + " is missing");
            } else if (hex != file.sha256 || size != file.size) {
                problems.push_back(path + " changed since the build (SHA-256 " + hex.substr(0, 12) + ", expected " + file.sha256.substr(0, 12) + ")");
            }
        }
        for (const ManifestPack& pack : manifest.packs) {
            std::string hex;
            uint64_t size = 0;
            if (access.sha256(pack.file, hex, size) && hex != pack.sha256)
                problems.push_back(pack.file + " changed since the build");
        }
    }
    return problems.empty();
}

std::string player::runtimePlayerId() {
#ifdef JASPERENGINE_PLAYER_ID
    return JASPERENGINE_PLAYER_ID;
#else
    return "";
#endif
}
