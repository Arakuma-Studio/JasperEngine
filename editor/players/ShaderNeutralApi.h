// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Lua API that cannot change what an object draws with (docs/specs/S34-player-size.md D5):
// a script that names only these, the Lua language and its own values can never ask the runtime
// shader compiler for a variant the export did not cook. Everything else is shader-relevant,
// so a new binding is relevant until it is reviewed here.
//
// - neutral classes: every member (math, input, time, logs, files, physics, audio, navigation,
//   the animators, scene loading and prefab spawning, transform tweens). Each comes with the
//   FNV-1a 32 of its member names (sorted, one per line), which ShaderNeutralApi.* holds against
//   the Lua API description: a member added to one of them fails the test until it is reviewed
//   and the hash updated.
// - mixed classes (Engine, Scene, GameObject, Camera): the members listed; the class names
//   themselves are not relevant (a script holds them through `self.scene` and the like), their
//   other members are.
//
// The export's collectors already cook what the scenes, the prefabs a script can spawn into them,
// and the scripts' light-capacity, shadow-quality and lit-quality switches reach
// (ShaderVariantCollector), so those Scene members are neutral too.

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace jasperengine::editor {

    class ShaderNeutralApi {
    public:
        // class name -> the hash of its reviewed member names
        static const std::map<std::string, uint32_t>& neutralClasses();
        // class name -> its neutral members
        static const std::map<std::string, std::set<std::string>>& mixedClasses();
        static bool isNeutralClass(const std::string& name);
        // FNV-1a 32 of the names, each followed by '\n', in set order
        static uint32_t memberHash(const std::set<std::string>& names);
    };

}
