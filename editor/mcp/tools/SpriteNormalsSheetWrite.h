// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// generate_sprite_normals' save_path contact sheet (SpriteNormalsTools.cpp), written before the
// project files (a sheet that can't be written fails the call before anything else is). Until
// commit(), a later failure (or an exception unwinding the finish) puts the path back as it was:
// its old bytes, or no file and none of the folders made for it. Written to a temporary file
// beside it, then renamed over it: a symbolic link at the path is replaced, never written through.
#pragma once

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

        // false with `error` when the sheet could not be written; the path is then put back (and
        // `warning`, when given, says so if its previous bytes could not be written back)
        bool write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string& error, std::string* warning = nullptr) {
            namespace fs = std::filesystem;
            std::error_code ec;
            target = path;
            existed = fs::is_regular_file(path, ec);
            if (existed) {
                std::ifstream in(path, std::ios::binary);
                previous.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            // the folders create_directories makes, deepest first (removed again on rollback)
            for (fs::path dir = path.parent_path(); !dir.empty() && !fs::exists(dir, ec); dir = dir.parent_path()) {
                created.push_back(dir);
                if (dir == dir.parent_path()) break;
            }
            active = true;
            if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
            // a path that could not be replaced was not touched: the rollback leaves it alone
            touched = replace(path, reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!touched) {
                error = ec ? ec.message() : std::string("the file could not be written");
                std::string restore;
                if (!rollback(&restore) && warning) *warning = restore;
                return false;
            }
            return true;
        }

        void commit() { committed = true; }

        // false (with `warning`) when the previous bytes could not be written back
        bool rollback(std::string* warning = nullptr) {
            namespace fs = std::filesystem;
            if (!active) return true;
            active = false;
            std::error_code ec;
            bool ok = true;
            if (touched && existed) {
                ok = replace(target, previous.data(), previous.size());
                if (!ok && warning) *warning = "the sheet replaced " + target.generic_u8string() + " and its previous version could not be written back";
            } else if (touched) {
                fs::remove(target, ec);
            }
            for (const fs::path& dir : created) fs::remove(dir, ec);   // only empty folders go
            return ok;
        }

        const std::filesystem::path& path() const { return target; }

    private:
        // `size` bytes into a temporary file beside `path`, renamed over it (what is at `path`,
        // a symbolic link included, is replaced, not written through); false (and no temporary
        // file left) when that failed
        static bool replace(const std::filesystem::path& path, const char* data, size_t size) {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::path name(".");
            name += path.filename();
            name += ".sheet.tmp";
            const fs::path temp = path.parent_path() / name;
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                if (!file || !file.write(data, static_cast<std::streamsize>(size)) || !(file.close(), file)) {
                    fs::remove(temp, ec);
                    return false;
                }
            }
            fs::rename(temp, path, ec);
            if (ec) {
                fs::remove(temp, ec);
                return false;
            }
            return true;
        }

        std::filesystem::path target;
        bool existed = false;
        bool touched = false;
        std::string previous;
        std::vector<std::filesystem::path> created;
        bool active = false;
        bool committed = false;
    };

}
