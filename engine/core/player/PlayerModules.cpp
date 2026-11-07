// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlayerModules.h"

#include "component/AudioSourceComponent.h"
#include "navigation/NavTypes.h"

#include <algorithm>

using namespace jasperengine;

const std::vector<std::string>& player::allModules() {
    static const std::vector<std::string> modules = {"htmlui", "video", "runtimeshaders", "physics3d", "physics2d", "navigation", "soundlayer", "titleart"};
    return modules;
}

std::vector<std::string> player::builtModules() {
    std::vector<std::string> modules;
#if JASPERENGINE_HTMLUI
    modules.push_back("htmlui");
#endif
#if JASPERENGINE_VIDEO
    modules.push_back("video");
#endif
#ifdef JASPERENGINE_RUNTIME_SHADERS
    modules.push_back("runtimeshaders");
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    modules.push_back("physics3d");
#endif
#ifdef JASPERENGINE_PHYSICS_2D
    modules.push_back("physics2d");
#endif
    if (nav::isNavMeshAvailable()) modules.push_back("navigation");
#if JASPERENGINE_SOUND_LAYER
    modules.push_back("soundlayer");
#endif
#if JASPERENGINE_TITLE_ART
    modules.push_back("titleart");
#endif
    return modules;
}

bool player::isModuleBuilt(const std::string& module) {
    const std::vector<std::string> modules = builtModules();
    return std::find(modules.begin(), modules.end(), module) != modules.end();
}

std::string player::moduleDisplayName(const std::string& module) {
    if (module == "htmlui") return "the HTML UI";
    if (module == "video") return "the HTML UI's video";
    if (module == "runtimeshaders") return "the runtime shader compiler";
    if (module == "physics3d") return "3D physics";
    if (module == "physics2d") return "2D physics";
    if (module == "navigation") return "navigation meshes";
    if (module == "soundlayer") return "Sound Events and clip import settings";
    if (module == "titleart") return "Title Art (Realtime and Glyph Atlas)";
    return module;
}
