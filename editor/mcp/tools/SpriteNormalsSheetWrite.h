// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// generate_sprite_normals' save_path contact sheet (SpriteNormalsTools.cpp), written before the
// project files (a sheet that can't be written fails the call before anything else is). Until
// commit(), a later failure (or an exception unwinding the finish) puts the path back as it was:
// its old bytes, or no file and none of the folders made for it. Written beneath the project
// (safewrite::writeFileResolvedBeneath: a temporary file of its own beside it, created
// exclusively, renamed over it; a failed write leaves the old file whole): the path is resolved
// first and refused when it leads outside the root, and no symbolic link planted on the way since
// the containment check is followed. A live link at save_path whose target is inside the project
// is written through; a dangling link there is replaced by the sheet, never followed. The
// rollback goes beneath the root the same way (the previous bytes written back, or the new file
// removed: the entry itself).
#pragma once

#include "util/SafeWrite.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace jasperengine::editor::mcp {

    class SheetWrite {
    public:
        SheetWrite() = default;
        SheetWrite(const SheetWrite&) = delete;
        SheetWrite& operator=(const SheetWrite&) = delete;
        ~SheetWrite() {
            if (!committed) rollback();
        }

        // false with `error` when the sheet could not be written beneath `root` (the project); the
        // path is then put back (and `warning`, when given, says so if its previous bytes could not
        // be written back)
        bool write(const std::filesystem::path& root, const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
                   std::string& error, std::string* warning = nullptr) {
            namespace fs = std::filesystem;
            std::error_code ec;
            base = root;
            target = path;
            existed = fs::is_regular_file(path, ec);
            if (existed) {
                std::ifstream in(path, std::ios::binary);
                previous.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            active = true;
            created.clear();
            // a path that could not be replaced was not touched (and no folder was left made for it)
            touched = safewrite::writeFileResolvedBeneath(base, path, std::string(bytes.begin(), bytes.end()), error, &created);
            if (!touched) {
                std::string restore;
                if (!rollback(&restore) && warning) *warning = restore;
                return false;
            }
            return true;
        }

        void commit() { committed = true; }

        // false (with `warning`) when the previous bytes could not be written back
        bool rollback(std::string* warning = nullptr) {
            if (!active) return true;
            active = false;
            bool ok = true;
            std::string why;
            if (touched && existed) {
                ok = safewrite::writeFileResolvedBeneath(base, target, previous, why);
                if (!ok && warning) *warning = "the sheet replaced " + target.generic_u8string() + " and its previous version could not be written back (" + why + ")";
            } else if (touched) {
                safewrite::removeFileResolvedBeneath(base, target, why);
            }
            // only empty folders go, deepest first
            for (auto it = created.rbegin(); it != created.rend(); ++it) safewrite::removeEmptyDirResolvedBeneath(base, *it, why);
            created.clear();
            return ok;
        }

        const std::filesystem::path& path() const { return target; }

    private:
        std::filesystem::path base;
        std::filesystem::path target;
        bool existed = false;
        bool touched = false;
        std::string previous;
        std::vector<std::filesystem::path> created;
        bool active = false;
        bool committed = false;
    };

}
