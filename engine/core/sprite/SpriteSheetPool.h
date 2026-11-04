// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shared sprite sheets (docs/specs/S44-aseprite-import.md R46, §6.11): one
// shared_ptr<const SpriteSheetData> per normalized path, with a generation that every
// invalidate (an editor reimport, an atlas rebuild) moves on, as AnimationClipPool does.
// - Sources, first match wins: the registered ones in registration order (S45: texture Sprite
//   Mode, "name@WxH", .spriteatlas), then the built-in ones: an .aseprite/.ase file's imported
//   record ("<path>#sheet" through ImportedAssets, its pages "<path>#image#<n>"), and any plain
//   texture (one frame, the whole image, pivot at its centre).
// - Keys: MeshSystem's model key rule (FileData::getSystemPath, lexically normal, '/'), so the
//   component's asset-relative path and an absolute one agree.
// - Thread-safety: every function takes the pool mutex; loads run outside it, and a load that an
//   invalidate overtook is returned to its caller but not cached. A sheet a caller holds stays
//   valid after invalidate/clear.
// - Failures are remembered until the next invalidate of the path (one log line, no retry per
//   frame); revision() moves on with any change, a cheap per-frame "anything new?".

#ifndef SPRITE_SHEET_POOL_H
#define SPRITE_SHEET_POOL_H

#include "Export.h"
#include "sprite/SpriteSheet.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace jasperengine{

    class JASPERENGINE_API SpriteSheetPool{
    public:
        // the cached sheet, else a load; null (with one log line) when no source gives one
        static std::shared_ptr<const SpriteSheetData> get(const std::string& path);
        // the cached sheet only (never loads)
        static std::shared_ptr<const SpriteSheetData> find(const std::string& path);
        // 0 until the path is first loaded; moves on with every invalidate and reload
        static uint64_t generation(const std::string& path);
        // drops the path's sheet (and its remembered failure): the next get() loads it again
        static void invalidate(const std::string& path);
        static void clear();
        static uint64_t revision();
        static std::string getKey(const std::string& path);

        using Loader = std::function<std::shared_ptr<const SpriteSheetData>(const std::string& path)>;
        static void registerSource(std::function<bool(const std::string& path)> accepts, Loader loader);

        // the built-in loaders, for callers that know the source (tools, tests)
        static std::shared_ptr<const SpriteSheetData> loadImported(const std::string& path, std::string* error);
        static std::shared_ptr<const SpriteSheetData> loadTexture(const std::string& path, std::string* error);
        // ".aseprite" / ".ase" (any case)
        static bool isAsepritePath(const std::string& path);
    };

}

#endif // SPRITE_SHEET_POOL_H
