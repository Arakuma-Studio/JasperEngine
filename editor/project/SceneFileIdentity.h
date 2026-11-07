// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The identity a .scene file carries at its root: the scene id (child-scene references, the
// build list and the generated code use it) and the scene name (unique in a project, it names
// the generated code). A copied file gets its own; so does a file whose id another scene already
// has when it loads (docs/data-safety.md, "Scene files and identity").

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace jasperengine::editor::sceneid {

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
