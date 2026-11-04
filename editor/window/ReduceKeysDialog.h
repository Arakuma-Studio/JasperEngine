// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Reduce Keys... (S74 R46): the dialog the clip asset Inspector opens. Error Tolerance and Skin
// Distance run animation/ClipOptimize on a worker as they change; the numbers shown, before and after
// Memory and Keys, Max Error, Mean Error and Worst Joint, are its measurements, under the field table's
// labels (the same ones optimize_animation returns). The preview writes nothing. Reduce writes
// a new .anim, or with Replace This Clip the source, as one project undo step, then reads the file
// back and shows what it holds. A gain can be small or none: the dialog says what it measured.
// Main thread; App draws it every frame.

#pragma once

#include <string>

namespace jasperengine::editor{

    class Project;

    class ReduceKeysDialog{
    public:
        // `reference`: a project-relative .anim or "<model>#<clip>"
        static void open(Project* project, const std::string& reference);
        static void draw(Project* project);
    };

}
