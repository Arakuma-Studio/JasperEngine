// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PREFAB_COMPONENT_H
#define PREFAB_COMPONENT_H

#include <string>

namespace jasperengine{

    struct PrefabComponent {
        std::string name;
        std::string path;   // project-relative .prefab path; with a guid, only a hint
        std::string guid;   // the prefab asset's GUID (editor AssetDatabase), empty if unknown
    };

}

#endif //PREFAB_COMPONENT_H
