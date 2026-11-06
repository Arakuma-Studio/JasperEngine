// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The identity a .scene file carries at its root: the scene id (child-scene references, the
// build list and the generated code use it) and the scene name (unique in a project, it names
// the generated code). A copied file gets its own; a clash found at load gets a replacement
// that is the same at every load (docs/data-safety.md, "Scene files and identity").

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace jasperengine::editor::sceneid {

    // A replacement id for the scene file at `relative` (project-relative): derived from the path,
    // so every load picks the same one (the file's GUID is not settled yet for a copy made outside
    // the editor: it carries its original's), and probed past the ids `taken` reports. Never 0
    // (NULL_PROJECT_SCENE).
    uint32_t replacementId(const std::filesystem::path& relative, const std::function<bool(uint32_t)>& taken);

    // The root `id` and `name` of a scene file's text; false when the text has no root id
    bool readSceneHeader(const std::string& text, uint32_t& id, std::string& name);

    // The text with its root `id:` and/or `name:` lines replaced (nested keys, line endings and
    // every other byte kept); false when a requested key has no root line
    bool rewriteSceneHeaderText(std::string& text, std::optional<uint32_t> id, const std::optional<std::string>& name);

    // rewriteSceneHeaderText on a file, written atomically; refused when the file changed since
    // it was read
    bool rewriteSceneHeader(const std::filesystem::path& file, std::optional<uint32_t> id, const std::optional<std::string>& name,
                            std::string* error);

}
