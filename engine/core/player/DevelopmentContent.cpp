// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DevelopmentContent.h"

#include "player/ContentManifest.h"

using namespace jasperengine;

namespace jasperengine::player {

std::optional<DevelopmentContent> readDevelopmentContent(const std::string& folder, bool contentGiven, const std::string& runningPlayerId,
                                                                 const std::function<std::string()>& packagedPlayerSha256,
                                                                 const DevelopmentContentAccess& access) {
    if (contentGiven || folder.empty()) return std::nullopt;
    const std::string manifestPath = folder + "/content-manifest.json";
    if (!access.exists(manifestPath)) return std::nullopt;
    auto another = [&access]() {
        if (access.warn) access.warn(std::string("development content in ") + DEVELOPMENT_CONTENT_DIR + " is for another player; using the packaged content");
        return std::nullopt;
    };
    std::string text, error;
    ContentManifest manifest;
    if (!access.read(manifestPath, text) || !ContentManifest::parse(text, manifest, error)) return another();
    // the binary identity of R6: the runtime id, and the hash of the player the package was built with
    if (manifest.playerId.empty() || manifest.playerId != runningPlayerId) return another();
    if (manifest.playerSha256 != packagedPlayerSha256()) return another();
    return DevelopmentContent{folder, manifest.patch};
}

}  // namespace jasperengine::player
