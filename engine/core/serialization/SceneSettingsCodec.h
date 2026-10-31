// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A cooked scene's settings record: what the generated create_<scene>() sets on its scene
// after its entities (Factory::createScene), through the same setters, in the same order.

#include "Export.h"
#include "serialization/CookStream.h"

namespace jasperengine {
    class Scene;
}

namespace jasperengine::cook {

    JASPERENGINE_API void writeSceneSettings(Writer& w, const Scene& scene);
    JASPERENGINE_API void readSceneSettings(Reader& r, Scene& scene);

}
