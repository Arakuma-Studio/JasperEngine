// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TITLE_ART_INSTANCE_H
#define TITLE_ART_INSTANCE_H

// The per-instance state TitleArtSystem keeps for a Title Art Renderer (S81 §6.1 `runtime`): what
// it resolved (sidecar, bake textures, exposed parameters), the companion it drives and what it
// saved of it, readiness and the events Lua subscribes to, and the module's live state for a
// Realtime or Glyph Atlas title. Made once per component and kept for its lifetime (Lua holds
// references to its events). Never saved. Main thread.

#include "Export.h"
#include "component/TitleArtRendererComponent.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/TitleArtRuntime.h"
#include "texture/Texture.h"
#include "util/FunctionSubscribe.h"
#include "util/TitleArtBakeInfo.h"
#include "asset/AssetRef.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::titleart{

    // where the companion puts the title (R1): read from the companion, never stored
    enum class TitleArtSpace : uint8_t { None, UI, World };

    // what the system wrote of the companion's saved fields while a Live Effect is on (R2):
    // put back when the effect goes or the component is removed, and what the editor saves
    struct JASPERENGINE_API TitleArtCompanionSaved{
        bool active = false;
        ShaderRef customShader;
        ShaderUniformValues shaderUniforms;
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        std::vector<std::string> shaderFeatures;
    };

    class JASPERENGINE_API TitleArtInstance{
    public:
        // ---- what was resolved (needUpdate) ----
        std::string resolvedTitle;
        TitleArtRenderMode resolvedMode = TitleArtRenderMode::BAKED;
        bool resolved = false;
        uint64_t resolvedRevision = 0;  // TitleArtSystem::notifyTitleChanged's count it resolved at
        bool prebakeChecked = false;
        bool hasBake = false;           // the sidecar (or the default main PNG) was found
        TitleArtBakeInfo bake;
        std::string bakePath;           // the main output PNG, project-relative
        std::string glowPath;           // the first `glow` output, "" when none
        Texture bakeTexture;
        Texture glowTexture;
        bool exposedKnown = false;
        std::vector<TitleArtExposedParam> exposed;
        std::string documentError;
        // R51: a Realtime or Glyph Atlas title in a build without the module
        bool moduleMissing = false;

        // ---- the companion (R1, R2) ----
        TitleArtSpace space = TitleArtSpace::None;
        bool lending = false;           // RenderSystem lends the display image this frame
        bool lendingRealtime = false;   // ... from the live state (else the bake)
        TitleArtCompanionSaved saved;
        uint64_t liveFeatures = 0;      // the title_fx features applied (bit per feature)
        std::vector<std::string> liveFeatureNames;

        // ---- status (R18, R19) ----
        bool ready = false;
        std::string pending;
        bool failed = false;
        std::string error;
        std::string pixelPath;
        uint64_t readyHash = 0;         // the state whose onReady fired
        uint64_t failedHash = 0;        // the state whose onFailed fired
        std::string missingReported;    // the character set onMissingCharacters last fired for

        // ---- events (Lua `onReady`...) ----
        FunctionSubscribe<void()> onReady;
        FunctionSubscribe<void(const std::string&)> onFailed;
        FunctionSubscribe<void(const std::string&)> onMissingCharacters;
        FunctionSubscribe<void(const std::string&)> onAnimationFinished;

        // ---- live layers (R33-R38) ----
        std::vector<TitleArtAnchor> anchors;
        bool liveTimeStarted = false;   // enabled once: liveTime starts at 0 (R38)
        bool wasEnabled = false;

        // ---- the module's state (Realtime, Glyph Atlas) ----
        std::shared_ptr<TitleArtLive> live;
        TitleArtLiveStatus liveStatus;
        bool liveRuns = false;          // its state renders now (visible, or Render When Hidden)
        uint32_t testFlags = 0;         // TITLE_ART_TEST_* copied into every request (R21)

        // ---- warnings said once (R7, R19, R41, R51) ----
        std::set<std::string> said;
        bool sayOnce(const std::string& key){ return said.insert(key).second; }
    };

}

#endif //TITLE_ART_INSTANCE_H
