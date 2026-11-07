// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S74 R27, R28, R40, D4)
// - Purpose: a model's per-clip import settings, Unity's clip Wrap Mode and Root Motion, in the
//   sidecar "<model>.clips" next to the model file ("models/hero.glb.clips"), as the .humanoid
//   sidecar keeps its Avatar. JSON: {"version": 1, "clips": {"Walk": {"wrap": "loop",
//   "root_motion": {"bone": "", "bake_rotation": false, "bake_position_y": true,
//   "bake_position_xz": false}}}}, clips by sub-clip name (subClipNames).
// - The editor and a player read the same file the same way (buildModelSubClip applies it), so
//   Play matches an export and no cook is involved. Not a .meta setting: metas do not ship.
// - Absent file, absent clip or absent field: today's values, byte for byte (wrap Once, no
//   root_motion block: the Animator's choice).
// - Unknown keys, at the top, in a clip and in its root_motion, survive a write.
// - Thread-safety: parse and write are pure. modelClipSettings and dropModelClipSettings may run on
//   any thread (one mutex): buildModelSubClip runs off the main thread.

#ifndef MODEL_CLIP_SETTINGS_H
#define MODEL_CLIP_SETTINGS_H

#include "Export.h"
#include "animation/AnimationClip.h"
#include "animation/AnimatorAssetIO.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::anim{

    constexpr int MODEL_CLIP_SETTINGS_VERSION = 1;

    using RawJsonMembers = std::vector<std::pair<std::string, std::string>>;    // key, its JSON text

    struct JASPERENGINE_API ModelClipSetting{
        std::optional<ClipWrapMode> wrap;           // none: the importer's Once; never Default
        std::optional<ClipRootMotion> rootMotion;   // none: no root_motion block
        RawJsonMembers unknownKeys;
        RawJsonMembers rootMotionUnknownKeys;

        bool empty() const { return !wrap && !rootMotion && unknownKeys.empty(); }
        bool operator==(const ModelClipSetting& other) const;
        bool operator!=(const ModelClipSetting& other) const { return !(*this == other); }
    };

    struct JASPERENGINE_API ModelClipSettings{
        std::map<std::string, ModelClipSetting> clips;
        RawJsonMembers unknownKeys;

        // the clip's setting; null when it has none
        const ModelClipSetting* find(const std::string& clip) const;
    };

    // "models/hero.glb" -> "models/hero.glb.clips"
    JASPERENGINE_API std::string modelClipSettingsPath(const std::string& modelPath);

    // Wrong values are warnings and read as absent; text that is not a JSON object or a version
    // above MODEL_CLIP_SETTINGS_VERSION fails
    JASPERENGINE_API bool parseModelClipSettings(const std::string& text, ModelClipSettings& out,
                                                 std::vector<std::string>& warnings, std::string* error);
    // Deterministic: version, clips by name (a clip with no setting left is not written), each
    // wrap then root_motion then its unknown keys, then the top's unknown keys; 2-space indent and
    // a trailing newline
    JASPERENGINE_API std::string writeModelClipSettings(const ModelClipSettings& settings);

    // The sidecar of a model, read once and kept until dropModelClipSettings: null when there is
    // none, or when it does not parse (a warning, once). One failed lookup per model, then cached.
    JASPERENGINE_API std::shared_ptr<const ModelClipSettings> modelClipSettings(const std::string& modelPath);
    // forgets the read of `modelPath` (every model when empty): dropModelFileParse calls it, so
    // AnimationClipPool::invalidateModel reads the sidecar again
    JASPERENGINE_API void dropModelClipSettings(const std::string& modelPath);

    // the setting on a model clip's data: its wrap, its root_motion block
    JASPERENGINE_API void applyModelClipSetting(const ModelClipSetting& setting, ClipData& data);

}

#endif //MODEL_CLIP_SETTINGS_H
