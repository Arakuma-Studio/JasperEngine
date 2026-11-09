// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteEditCmd.h"

#include "Project.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "pixelart/draw/SpriteDocCache.h"
#include "pixelart/draw/SpriteEditEvents.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/SafeWrite.h"

namespace fs = std::filesystem;

namespace jasperengine::editor {

    using namespace pixelart::draw;

    SpriteEditCmd::SpriteEditCmd(Project* p, fs::path file, std::shared_ptr<const SpritePatchSet> set, std::shared_ptr<const std::vector<uint8_t>> bytes, SpriteDoc written)
        : project(p), absolute(std::move(file)), fileName(absolute.filename().generic_string()), patch(std::move(set)), firstBytes(std::move(bytes)), firstDoc(std::move(written)) {
        mergeable = false;
    }

    std::vector<Command::Touched> SpriteEditCmd::touchedPaths(bool) const {
        return {{absolute, false}};
    }

    std::string SpriteEditCmd::fileConflict(const std::string& want, const char* verb) const {
        pixelart::PixelProblem problem;
        std::shared_ptr<SpriteSession> session = SpriteDocCache::get().openAbsolute(project, absolute, problem);
        if (!session) return fileName + " can no longer be read; " + verb + " would overwrite it";
        if (session->revision != want) return fileName + " changed outside the editor after this step; " + verb + " would overwrite that change";
        return std::string();
    }

    std::string SpriteEditCmd::undoRefusal() const {
        const std::string why = Command::undoRefusal();
        return why.empty() ? fileConflict(patch->revisionAfter, "undoing") : why;
    }

    std::string SpriteEditCmd::redoRefusal() const {
        const std::string why = Command::redoRefusal();
        return why.empty() ? fileConflict(patch->revisionBefore, "redoing") : why;
    }

    std::string SpriteEditCmd::describe() const {
        return "Edit Sprite (" + fileName + (patch->summary.empty() ? "" : ": " + patch->summary) + ")";
    }

    std::string SpriteEditCmd::cachedRefusal(bool undo) const {
        std::shared_ptr<SpriteSession> session = SpriteDocCache::get().find(absolute);
        if (!session) return std::string();
        if (session->revision != (undo ? patch->revisionAfter : patch->revisionBefore)) {
            return fileName + " changed outside the editor after this step; " + (undo ? "undoing" : "redoing") + " would overwrite that change";
        }
        return std::string();
    }

    size_t SpriteEditCmd::memoryBytes() const {
        return patch->memoryBytes() + (firstBytes ? firstBytes->size() : 0);
    }

    bool SpriteEditCmd::writeFile(const std::vector<uint8_t>& bytes, std::string& why) {
        const std::string content(bytes.begin(), bytes.end());
        const fs::path root = rootFor(absolute);
        if (!root.empty()) {
            why = safewrite::scopesRefusal(absolute, false);
            if (!why.empty()) return false;
            return safewrite::writeFileResolvedBeneath(root, absolute, content, why);
        }
        if (!FileUtils::writeFileReplacing(absolute, content)) {
            why = "can't write " + FileUtils::pathToGenericUtf8(absolute);
            return false;
        }
        return true;
    }

    void SpriteEditCmd::afterWrite(const char* origin, bool announce) {
        // no openAbsolute here: this is the editor's own write, the session already holds it
        std::shared_ptr<SpriteSession> session = SpriteDocCache::get().find(absolute);
        // the first execute: the tool announces it, with the history entry the step becomes (R41)
        if (session && announce) {
            SpriteEdited event;
            event.path = session->path;
            event.revision = session->revision;
            event.rects = patch->rects;
            event.origin = origin;
            event.by = patch->by;
            event.summary = patch->summary;
            SpriteEditEvents::edited(event);
        }
        // S05's self-write suppression hides these writes from the watcher (R41)
        if (!isPngPath(absolute) && session) pixelart::aseprite::AsepriteImporter::get().reimport(session->assetPath);
        ProjectFileWrites::filesWritten(project, {absolute});
    }

    bool SpriteEditCmd::apply(bool forward, std::string& why) {
        pixelart::PixelProblem problem;
        std::shared_ptr<SpriteSession> session = SpriteDocCache::get().openAbsolute(project, absolute, problem);
        if (!session) {
            why = problem.message;
            return false;
        }
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        SpriteDoc sprite;
        if (!forward && patch->beforeFile) {
            bytes = patch->beforeFile;
        } else {
            sprite = session->sprite;
            applyPatch(sprite, *patch, forward);
            auto encoded = std::make_shared<std::vector<uint8_t>>();
            if (!encodeSprite(sprite, *encoded, problem)) {
                why = problem.message;
                return false;
            }
            bytes = encoded;
        }
        if (!writeFile(*bytes, why)) return false;
        const std::string revision = SpriteDocCache::revisionOf(*bytes);
        SpriteDoc fresh;
        if (!decodeSprite(absolute, *bytes, fresh, problem)) {
            why = problem.message;
            return false;
        }
        const std::string previous = session->revision;
        SpriteDocCache::get().adoptWrite(*session, std::move(fresh), bytes, revision);
        // an undo moves the file for everyone who was looking at it: not a change behind their back (R34)
        for (auto& [client, seen] : session->lastSeen) {
            if (seen == previous) seen = revision;
        }
        return true;
    }

    bool SpriteEditCmd::execute() {
        lastFailure.clear();
        if (firstBytes) {
            std::string why;
            if (!writeFile(*firstBytes, why)) {
                lastFailure = why;
                if (failureSink) *failureSink = why;
                return false;
            }
            std::shared_ptr<SpriteSession> session = SpriteDocCache::get().find(absolute);
            if (session) {
                SpriteDocCache::get().adoptWrite(*session, std::move(firstDoc), firstBytes, patch->revisionAfter);
                SpriteDocCache::get().addPatch(*session, patch);
            }
            firstBytes.reset();
            firstDoc = SpriteDoc();
            afterWrite("agent", false);
            return true;
        }
        std::string why;
        if (!apply(true, why)) {
            lastFailure = why;
            return false;
        }
        afterWrite("redo", true);
        return true;
    }

    void SpriteEditCmd::undo() {
        std::string why;
        if (!apply(false, why)) {
            undoProblem = why;
            return;
        }
        afterWrite("undo", true);
    }

}
