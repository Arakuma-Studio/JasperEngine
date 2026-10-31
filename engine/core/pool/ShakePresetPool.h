// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 5.7)
// - Purpose: the parsed .shake presets by path. They are tiny and load synchronously through
//   Data::open (asset-relative, asset://, absolute paths; the resource pack in exported builds).
// - Staleness: an entry remembers the file's size and modification time when the file is on disk
//   (the editor, desktop builds) and loads again when they change, so a preset edited in the
//   editor or outside it plays its new values on the next shake. invalidate() drops one path.
// - A path that cannot be read or parsed is not cached (it may be written later); the error says
//   why (with the JSON line:column).
// - Main thread (engine); Engine::clearPools clears it.

#ifndef SHAKE_PRESET_POOL_H
#define SHAKE_PRESET_POOL_H

#include "Export.h"
#include "camera/ShakePreset.h"

#include <memory>
#include <string>

namespace jasperengine{

    class JASPERENGINE_API ShakePresetPool{
    public:
        // the key of a path: generic separators, lexically normal, no "./" or "asset://" prefix
        static std::string getKey(const std::string& path);
        // the preset of `path`, loading it (again) when needed; nullptr and `error` when it can't be read
        static std::shared_ptr<const camera::ShakePresetFile> get(const std::string& path, std::string* error = nullptr);
        static void invalidate(const std::string& path);
        static void clear();
        static size_t size();
    };

}

#endif //SHAKE_PRESET_POOL_H
