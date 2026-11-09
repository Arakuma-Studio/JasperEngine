// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteDocCache.h"

#include "SpriteEditEvents.h"

#include "Project.h"
#include "asset/Guid.h"
#include "util/FileUtils.h"
#include "util/Sha256.h"

#include "Out.h"
#include "io/watch/FileWatcher.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace jasperengine::editor::pixelart::draw {

    namespace {

        std::shared_ptr<const std::vector<uint8_t>> readAll(const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return nullptr;
            return std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        size_t spriteMemory(const SpriteDoc& sprite) {
            size_t total = sizeof(SpriteDoc);
            for (const aseprite::Cel& cel : sprite.doc.cels) {
                if (cel.image) total += cel.image->pixels().size() + cel.image->indices().size();
            }
            return total;
        }

        std::string fileName(const std::string& path) {
            return fs::path(path).filename().generic_string();
        }

    }

    SpriteDocCache& SpriteDocCache::get() {
        static SpriteDocCache cache;
        return cache;
    }

    std::string SpriteDocCache::revisionOf(std::span<const uint8_t> bytes) {
        return sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).substr(0, 12);
    }

    void SpriteDocCache::attach(Project* p) {
        if (project == p) return;
        clear();
        project = p;
        if (!project) return;
        // R39: nothing runs between calls but this subscription's deliveries
        WatchFilter filter;
        filter.root = project->getProjectPath();
        filter.extensions = {".aseprite", ".ase", ".png"};
        watcher = FileWatchService::get().subscribe(std::move(filter), [this](std::span<const FileChange> changes) {
            for (const FileChange& change : changes) {
                if (change.directory || change.origin != ChangeOrigin::External) continue;
                auto it = sessions.find(change.path.lexically_normal().generic_string());
                if (it == sessions.end()) continue;
                PixelProblem ignored;
                if (!sync(*it->second, ignored)) sessions.erase(it);
            }
        });
    }

    void SpriteDocCache::clear() {
        watcher.reset();
        sessions.clear();
        project = nullptr;
    }

    void SpriteDocCache::forget(const fs::path& absolute) {
        sessions.erase(absolute.lexically_normal().generic_string());
    }

    std::optional<LastEdit> SpriteDocCache::lastEditOf(const fs::path& absolute) const {
        auto it = sessions.find(absolute.lexically_normal().generic_string());
        return it == sessions.end() ? std::nullopt : it->second->lastEdit;
    }

    std::shared_ptr<SpriteSession> SpriteDocCache::open(Project* p, const std::string& given, PixelProblem& problem) {
        attach(p);
        std::string relative = given;
        if (Guid::isValidText(given)) {
            const AssetDatabase::Resolution found = p->resolveAsset(given, "", AssetType::Unknown);
            if (!found.found()) {
                problem = {"sprite_missing", "sprite", "no asset has the GUID " + given, "pass a project-relative path of an .aseprite, .ase or .png file (list_assets finds them)"};
                return nullptr;
            }
            relative = found.path;
        }
        const fs::path absolute = fs::path(relative).is_absolute() ? fs::path(relative).lexically_normal() : (p->getProjectPath() / fs::path(relative)).lexically_normal();
        return openAbsolute(p, absolute, problem);
    }

    std::shared_ptr<SpriteSession> SpriteDocCache::openAbsolute(Project* p, const fs::path& given, PixelProblem& problem) {
        attach(p);
        const fs::path absolute = given.lexically_normal();
        const std::string assetPath = absolute.lexically_relative(p->getAssetsPath().lexically_normal()).generic_string();
        const std::string path = absolute.lexically_relative(p->getProjectPath().lexically_normal()).generic_string();
        if (assetPath.empty() || assetPath.rfind("..", 0) == 0) {
            problem = {"outside_assets", "sprite", given.generic_string() + " is outside the assets directory", "sprites live under " + p->getAssetsPath().generic_string()};
            return nullptr;
        }
        if (!isSpritePath(absolute)) {
            problem = {"not_a_sprite", "sprite", path + " is not an .aseprite, .ase or .png file", "pass one of those (list_assets finds them)"};
            return nullptr;
        }
        return openResolved(absolute, path, assetPath, problem);
    }

    std::shared_ptr<SpriteSession> SpriteDocCache::openResolved(const fs::path& absolute, const std::string& path, const std::string& assetPath, PixelProblem& problem) {
        std::error_code ec;
        if (!fs::is_regular_file(absolute, ec)) {
            forget(absolute);
            problem = {"sprite_missing", "sprite", "no file " + path, "pass a project-relative path of an .aseprite, .ase or .png file (list_assets finds them)"};
            return nullptr;
        }
        const std::string key = absolute.generic_string();
        auto it = sessions.find(key);
        std::shared_ptr<SpriteSession> session;
        if (it == sessions.end()) {
            session = std::make_shared<SpriteSession>();
            session->path = path;
            session->assetPath = assetPath;
            session->absolute = absolute;
            if (!sync(*session, problem)) return nullptr;
            sessions[key] = session;
        } else {
            session = it->second;
            if (!sync(*session, problem)) {
                sessions.erase(key);
                return nullptr;
            }
        }
        session->lastUse = ++clock;
        evict(session.get());
        return session;
    }

    bool SpriteDocCache::sync(SpriteSession& session, PixelProblem& problem) {
        watch::PathStat stat;
        if (!watch::statPath(session.absolute, stat, false)) {
            problem = {"sprite_missing", "sprite", "no file " + session.path, "pass a project-relative path of an .aseprite, .ase or .png file (list_assets finds them)"};
            return false;
        }
        if (!session.revision.empty() && stat.size == session.size && stat.mtimeNs == session.mtimeNs) return true;
        std::shared_ptr<const std::vector<uint8_t>> bytes = readAll(session.absolute);
        if (!bytes) {
            problem = {"sprite_missing", "sprite", "can't read " + session.path, "check the file exists and is readable"};
            return false;
        }
        const std::string revision = revisionOf(*bytes);
        if (revision == session.revision) {
            session.size = stat.size;
            session.mtimeNs = stat.mtimeNs;
            return true;
        }
        SpriteDoc fresh;
        if (!decodeSprite(session.absolute, *bytes, fresh, problem)) return false;
        if (!session.revision.empty()) {
            overwrittenBy(session, fresh);
            session.replaced = std::make_shared<const SpriteDoc>(std::move(session.sprite));
            session.replacedRevision = session.revision;
            session.changedExternally = true;
        }
        session.sprite = std::move(fresh);
        session.bytes = std::move(bytes);
        session.revision = revision;
        session.size = stat.size;
        session.mtimeNs = stat.mtimeNs;
        session.memory = spriteMemory(session.sprite) + session.bytes->size();
        return true;
    }

    void SpriteDocCache::overwrittenBy(SpriteSession& session, const SpriteDoc& fresh) {
        // walk back from the state the save replaced through the agent's steps (R34.3)
        std::vector<std::shared_ptr<const SpritePatchSet>> lost;
        std::string revision = session.revision;
        for (size_t guard = 0; guard < session.patches.size(); guard++) {
            std::shared_ptr<const SpritePatchSet> found;
            for (auto it = session.patches.rbegin(); it != session.patches.rend(); ++it) {
                std::shared_ptr<const SpritePatchSet> patch = it->lock();
                if (patch && patch->revisionAfter == revision) {
                    found = patch;
                    break;
                }
            }
            if (!found) break;
            if (!patchSurvives(fresh, *found)) lost.push_back(found);
            revision = found->revisionBefore;
        }
        if (lost.empty()) return;
        std::string who;
        for (const auto& patch : lost) {
            const std::string piece = patch->by + ": " + patch->summary;
            if (who.find(piece) == std::string::npos) who += (who.empty() ? "" : "; ") + piece;
        }
        const std::string name = fileName(session.path);
        const std::string message = name + " was saved from outside the editor over " + std::to_string(lost.size()) + " agent edit" + (lost.size() == 1 ? "" : "s") + " (" + who + "); those steps can no longer be undone";
        Out::warning("%s", message.c_str());
        SpriteEditEvents::externalSave({session.path, static_cast<int>(lost.size()), message});
    }

    void SpriteDocCache::adoptWrite(SpriteSession& session, SpriteDoc sprite, std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string& revision) {
        watch::PathStat stat;
        if (watch::statPath(session.absolute, stat, false)) {
            session.size = stat.size;
            session.mtimeNs = stat.mtimeNs;
        }
        session.sprite = std::move(sprite);
        session.bytes = std::move(bytes);
        session.revision = revision;
        session.changedExternally = false;
        session.memory = spriteMemory(session.sprite) + session.bytes->size();
        evict(&session);
    }

    void SpriteDocCache::addPatch(SpriteSession& session, const std::shared_ptr<const SpritePatchSet>& patch) {
        session.patches.erase(std::remove_if(session.patches.begin(), session.patches.end(), [](const std::weak_ptr<const SpritePatchSet>& p) { return p.expired(); }),
                              session.patches.end());
        session.patches.push_back(patch);
    }

    void SpriteDocCache::evict(const SpriteSession* keep) {
        for (;;) {
            size_t bytes = 0;
            SpriteSession* oldest = nullptr;
            std::string oldestKey;
            for (auto& [key, session] : sessions) {
                bytes += session->memory;
                if (session.get() != keep && (!oldest || session->lastUse < oldest->lastUse)) {
                    oldest = session.get();
                    oldestKey = key;
                }
            }
            if (!oldest || (sessions.size() <= kMaxDocuments && bytes <= kMaxBytes)) return;
            sessions.erase(oldestKey);
        }
    }

    std::optional<SpriteDoc> SpriteDocCache::rebuild(const SpriteSession& session, const std::string& revision) const {
        if (revision == session.revision) return session.sprite;
        if (revision == session.replacedRevision && session.replaced) return *session.replaced;
        // breadth first over the live patches, from the current state
        struct Step {
            std::string revision;
            SpriteDoc sprite;
        };
        std::deque<Step> frontier;
        frontier.push_back({session.revision, session.sprite});
        std::set<std::string> seen{session.revision};
        while (!frontier.empty()) {
            Step step = std::move(frontier.front());
            frontier.pop_front();
            for (const std::weak_ptr<const SpritePatchSet>& weak : session.patches) {
                std::shared_ptr<const SpritePatchSet> patch = weak.lock();
                if (!patch) continue;
                const bool back = patch->revisionAfter == step.revision && !seen.count(patch->revisionBefore);
                const bool forth = patch->revisionBefore == step.revision && !seen.count(patch->revisionAfter);
                if (!back && !forth) continue;
                SpriteDoc next = step.sprite;
                applyPatch(next, *patch, forth);
                const std::string at = forth ? patch->revisionAfter : patch->revisionBefore;
                if (at == revision) return next;
                seen.insert(at);
                frontier.push_back({at, std::move(next)});
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> SpriteDocCache::knownRevisions(const SpriteSession& session) const {
        std::vector<std::string> out{session.revision};
        std::set<std::string> seen{session.revision};
        if (session.replaced && seen.insert(session.replacedRevision).second) out.push_back(session.replacedRevision);
        for (size_t i = session.patches.size(); i-- > 0;) {
            std::shared_ptr<const SpritePatchSet> patch = session.patches[i].lock();
            if (!patch) continue;
            for (const std::string& r : {patch->revisionBefore, patch->revisionAfter}) {
                if (seen.insert(r).second) out.push_back(r);
            }
        }
        return out;
    }

    std::shared_ptr<const SpritePatchSet> SpriteDocCache::newestPatch(const SpriteSession& session) const {
        for (auto it = session.patches.rbegin(); it != session.patches.rend(); ++it) {
            std::shared_ptr<const SpritePatchSet> patch = it->lock();
            if (patch && patch->revisionAfter == session.revision) return patch;
        }
        return nullptr;
    }

}
