// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An .aseprite file's tag clips as read-only sub-clips (docs/specs/S44-aseprite-import.md R16,
// §6.5): "<file>.aseprite#<tag>" like a model's "<model>#<clip>". ModelSubClips' reference split,
// clip list and builder call these for Aseprite containers, so AnimationClipPool, the Animation
// Player and the editor's read-only clip documents treat both alike. The clips are the import's
// generated .anim texts, found through ImportedAssets and parsed with anim::parseClip: the codec
// is never involved (it is editor-only). Thread-safety: as SpriteSheetPool and ImportedAssets.

#ifndef ASEPRITE_CLIPS_H
#define ASEPRITE_CLIPS_H

#include "Export.h"
#include "animation/ModelSubClips.h"

#include <string>
#include <vector>

namespace jasperengine::anim{

    // ".aseprite" / ".ase" (any case)
    JASPERENGINE_API bool isAsepriteClipContainer(const std::string& path);
    // the file's clips in sheet order (its tags, or the one clip of every frame named after the
    // file), names as the references use them; false when the file has no import yet
    JASPERENGINE_API bool listAsepriteClips(const std::string& path, std::vector<ModelClipInfo>& out, std::string* error);
    // one clip by name (or "<index>" when no clip has that name)
    JASPERENGINE_API bool buildAsepriteClip(const std::string& path, const std::string& clip, ClipData& out,
                                            std::vector<ClipWarning>& warnings, std::string* error);

}

#endif // ASEPRITE_CLIPS_H
