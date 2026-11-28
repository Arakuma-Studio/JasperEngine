// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R33, R46, R47)
// - Purpose: the shader variants a shipped Title Art Renderer can draw and render with, for the
//   export's collection and warm-up (S32): the title_fx / title_fx_sprite feature sets of its
//   Live Effects (with and without CROSSFADE for a Realtime title that crossfades, and with ANIM
//   and the effects its live.* tracks turn on while one of its document's animations plays,
//   S85 R13, R16), and the
//   ShaderType::TITLE_ART programs its document's render job binds on both pixel paths (only one
//   runs, but the device decides at runtime, as with fluid's linear filter).
// - Thread-safety: main thread (the renderer plans the jobs).

#pragma once

#include "component/TitleArtRendererComponent.h"
#include "shader/ShaderBuildTypes.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // the title_fx feature sets the component can draw with (none without a Live Effect or an
    // animation); its document's animations are read under `projectRoot`
    std::vector<std::vector<std::string>> titleArtLiveFeatureSets(const TitleArtRendererComponent& title, const std::filesystem::path& projectRoot);

    // the TITLE_ART program keys of a document's render (float and RGBA8 paths, its glow output
    // too); false with `error` when the document does not read
    bool addTitleArtRenderKeys(const std::filesystem::path& projectRoot, const std::string& titlePath, std::set<ShaderKey>& keys,
                               std::string* error = nullptr);

}
