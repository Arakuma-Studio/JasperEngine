// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The built-in pose templates embedded in the editor: editor/resources/pose-templates/*.poseanim,
// turned into pose_templates.h by CMakeLists.txt at configure time. A separate unit so the pose
// unit tests can compile PoseTemplates.cpp with their own copy read from the source folder.

#include "PoseTemplates.h"

#include "pose_templates.h"

using namespace jasperengine;

std::vector<editor::PoseTemplateResource> editor::PoseTemplates::builtinResources(){
    std::vector<PoseTemplateResource> out;
    for (const PoseTemplateEmbeddedFile& file : pose_template_embedded_files){
        if (!file.name){
            continue;   // the terminator of an empty list
        }
        out.push_back({file.name, std::string(reinterpret_cast<const char*>(file.data), file.size)});
    }
    return out;
}
