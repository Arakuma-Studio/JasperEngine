// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "component/PrefabComponent.h"

namespace jasperengine::editor{

    // A prefab instance unpacked by Project::unpackPrefabInstances, with what putting its
    // prefab link back takes: the members stay in the scene, only the bookkeeping goes
    struct PrefabUnpackRecord {
        std::filesystem::path path;       // prefab asset the instance followed
        size_t index = 0;                 // position in the asset's instance list for the scene
        Prefab::Instance instance;        // members, registry mapping and overrides
        bool hadComponent = false;
        PrefabComponent component;        // the root's PrefabComponent
        std::vector<PrefabNestedState> nestedBefore;
    };

}
