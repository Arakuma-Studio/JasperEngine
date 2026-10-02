// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PREFABASSET_H
#define PREFABASSET_H

#include "Export.h"
#include <cstdint>
#include <string>

namespace jasperengine {

    // A reference to a prefab asset: what a script property of type Prefab holds, and what
    // PrefabAsset::load returns. It names the prefab by its asset GUID (stable across renames
    // and moves) and by its project-relative path (a hint, used when there is no GUID). The
    // prefab itself is found in PrefabManager, which the editor's Play and the exported game
    // fill with every prefab of the build.
    //
    //   local enemy = PrefabAsset.load("enemies/Enemy")       -- path, with or without .prefab
    //   local clone = GameObject.instantiate(enemy, Vector3(0, 2, 0), Quaternion())
    class JASPERENGINE_API PrefabAsset {
    private:
        std::string guid;
        std::string path;

        // PrefabManager id found last time, valid while the registry is unchanged
        mutable uint32_t cachedId = 0;
        mutable uint32_t cachedRevision = 0;

    public:
        PrefabAsset();
        PrefabAsset(const std::string& guid, const std::string& path);

        // A prefab registered in PrefabManager, by GUID, by path ("enemies/Enemy" or
        // "enemies/Enemy.prefab") or by file name when only one prefab has it. An empty
        // PrefabAsset (isValid() false) when there is none, with an error logged.
        static PrefabAsset load(const std::string& nameOrPath);
        static PrefabAsset load(uint32_t id);

        // The PrefabManager id of the prefab, 0 when it is not registered
        uint32_t getId() const;
        bool isValid() const;  // registered: it can be instantiated
        bool isEmpty() const;  // references nothing (a script property left at None)

        const std::string& getGuid() const;
        const std::string& getPath() const;
        std::string getName() const; // the file name without extension

        bool operator==(const PrefabAsset& other) const;
        bool operator!=(const PrefabAsset& other) const;
    };

}

#endif // PREFABASSET_H
