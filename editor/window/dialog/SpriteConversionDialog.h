// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Assets > Convert Inline Sprite Frames... (docs/specs/S45-sprite-atlas.md R21): the dry-run list
// of the project's legacy sprites, grouped by texture (open and closed scenes, prefabs), and their
// conversion to texture sheets, one undo step per texture.

#pragma once

#include <map>
#include <memory>
#include <string>

namespace jasperengine::editor {

    class Project;

    class SpriteConversionDialog {
    public:
        void open(Project* project);
        void show();
        bool isOpen() const { return openRequested || visible; }

    private:
        Project* project = nullptr;
        bool openRequested = false;
        bool visible = false;
        // texture -> its legacy sprites (in the .cpp: App.h includes this header, and a sprite::
        // type here would make `sprite` a namespace in every source that includes App.h)
        struct Scan;
        std::shared_ptr<Scan> found;
        std::map<std::string, std::string> results;   // texture -> what converting it did, or why not

        void scan();
    };

}
