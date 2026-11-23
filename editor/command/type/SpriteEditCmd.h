// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"

#include "pixelart/draw/SpriteDoc.h"
#include "pixelart/draw/SpritePatch.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // One edit_sprite call on the project history (docs/specs/S47-agent-sprite-drawing.md R32, R33,
    // D7): dirty-rect patches, re-encoded through S44's writer and written whole and atomically on
    // every apply, after checking the file first. A step whose file no longer holds the revision
    // it left behind (a person saved it from Aseprite) is refused instead of overwriting that save.
    class SpriteEditCmd : public Command {
    public:
        // The first execute writes `bytes` (decoded as `written`); undo and redo apply the patch
        SpriteEditCmd(Project* project, std::filesystem::path absolute, std::shared_ptr<const pixelart::draw::SpritePatchSet> patch,
                      std::shared_ptr<const std::vector<uint8_t>> bytes, pixelart::draw::SpriteDoc written);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        std::vector<Touched> touchedPaths(bool undo) const override;
        std::string undoRefusal() const override;
        std::string redoRefusal() const override;
        // "Edit Sprite (hero.aseprite: line, fill x2)"
        std::string describe() const override;
        const char* typeName() const override { return "SpriteEditCmd"; }
        // the patch bytes (R32): what 1,000 one-pixel edits cost
        size_t memoryBytes() const override;
        // frees pixel buffers and strings only
        bool releaseOffThread() const override { return true; }
        std::string failure() const override { return lastFailure; }
        // Where execute() writes why it failed: the history deletes a failed command, the sink outlives it
        void reportFailureTo(std::shared_ptr<std::string> sink) { failureSink = std::move(sink); }
        // why undo or redo would be refused now, from what the session knows (no stat): the
        // Undo History window asks every frame
        std::string cachedRefusal(bool undo) const;

    private:
        Project* project;
        std::filesystem::path absolute;
        std::string fileName;
        std::shared_ptr<const pixelart::draw::SpritePatchSet> patch;
        // the first execute only
        std::shared_ptr<const std::vector<uint8_t>> firstBytes;
        pixelart::draw::SpriteDoc firstDoc;
        std::string lastFailure;
        std::shared_ptr<std::string> failureSink;

        // `want` is the revision the file must hold; "" with the sentence naming why not
        std::string fileConflict(const std::string& want, const char* verb) const;
        bool apply(bool forward, std::string& why);
        bool writeFile(const std::vector<uint8_t>& bytes, std::string& why);
        void afterWrite(const char* origin, bool announce);
    };

}
