#pragma once
#include "registry/EntityRegistry.h"
#include <vector>
namespace jasperengine::editor {
struct SceneProject {
    EntityRegistry* scene;
    std::vector<Entity> entities;
};
}
