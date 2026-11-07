// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R4, §6.1 "Why `overrides` is a keyed collection")
// - Purpose: the Title Art Renderer's `overrides` as one property per Exposed parameter,
//   "overrides.<name>", the way Catalog::splitShaderEntryProperty makes each shader entry its
//   own property: set_properties, prefab overrides (Catalog::copyPropertyValue), undo and the
//   Inspector address an entry by name, so a prefab variant that overrides `text` alone keeps
//   following its base's color scheme.
// - The property of an entry: text, font, image and color_scheme a String (the entry's text);
//   number and bool a Float (the value's x, bool 0/1); color a Vector4 (linear rgb, alpha 1).
// - An entry whose name the document no longer exposes is kept (R4): nothing here deletes one
//   unless asked (removeTitleArtOverride).
// - Thread-safety: main thread.

#pragma once

#include "Catalog.h"
#include "component/TitleArtRendererComponent.h"
#include "mcp/McpJson.h"

#include "yaml-cpp/yaml.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    constexpr const char* TITLE_ART_OVERRIDES_PREFIX = "overrides.";

    // "overrides.<name>" -> name; false for any other property
    bool splitTitleArtOverrideProperty(const std::string& property, std::string& name);

    // the property of one entry (no ref when there is none of that name)
    PropertyData titleArtOverrideProperty(TitleArtRendererComponent& title, const std::string& property);
    void listTitleArtOverrideProperties(TitleArtRendererComponent& title, std::map<std::string, PropertyData>& out);

    TitleArtOverride* findTitleArtOverride(TitleArtRendererComponent& title, const std::string& name);
    const TitleArtOverride* findTitleArtOverride(const TitleArtRendererComponent& title, const std::string& name);
    // removes the entry; false when there was none
    bool removeTitleArtOverride(TitleArtRendererComponent& title, const std::string& name);
    // the target's entry becomes the source's (made when missing, removed when the source has none):
    // a prefab override of "overrides.<name>"
    void copyTitleArtOverride(const TitleArtRendererComponent& source, TitleArtRendererComponent& target, const std::string& name);

    // set_properties' "overrides.<name>" values written by name into a component's YAML (its
    // `overrides` list): a null value removes the entry; the type is the document's exposed
    // type. False with `error` (an unknown name lists the exposed ones)
    bool applyTitleArtOverrideEdits(const TitleArtRendererComponent& title, YAML::Node& component,
                                    const std::vector<std::pair<std::string, mcp::Json>>& edits, std::string& error);

    // an entry's value as MCP shows it: a string, a number, a boolean or "#rrggbb" (sRGB)
    mcp::Json titleArtOverrideToJson(const TitleArtOverride& entry);
    // an MCP value into an entry of `type` (the document's exposed type): false with `error`
    bool titleArtOverrideFromJson(const mcp::Json& value, const std::string& type, TitleArtOverride& entry, std::string& error);

}

// the reflection's hooks (TitleArtRendererReflect.cpp)
namespace jasperengine::editor::reflect {
    using jasperengine::editor::titleArtOverrideProperty;
    using jasperengine::editor::listTitleArtOverrideProperties;
}
