// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseFile.h"

#include "io/Data.h"
#include "io/File.h"
#include "io/FileData.h"
#include "io/AtomicFile.h"

#include <filesystem>

namespace jasperengine::pose {

    namespace {

        namespace fs = std::filesystem;

        bool fail(std::string* error, const std::string& message){
            if (error){
                *error = message;
            }
            return false;
        }

        // A std::filesystem path from a UTF-8 string (also correct on Windows)
        fs::path utf8Path(const std::string& text){
#if defined(__cpp_lib_char8_t)
            return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
            return fs::u8path(text);
#endif
        }

    }

    bool readTextFile(const std::string& path, std::string& text, std::string* error, bool* notFound){
        if (notFound){
            *notFound = false;
        }
        // a directory opens as a file on some platforms (and reads as garbage): it exists, unreadable
        std::error_code directoryCode;
        if (fs::is_directory(utf8Path(FileData::getSystemPath(path)), directoryCode)){
            return fail(error, "cannot read '" + path + "'");
        }
        Data data;
        const unsigned int result = data.open(path.c_str());
        if (result != FileErrors::FILEDATA_OK){
            std::error_code code;
            const bool exists = result == FileErrors::FILE_NOT_FOUND &&
                                fs::exists(utf8Path(FileData::getSystemPath(path)), code);
            if (result == FileErrors::FILE_NOT_FOUND && !exists && notFound){
                *notFound = true;
            }
            return fail(error, (result == FileErrors::FILE_NOT_FOUND && !exists ? "file not found: '" : "cannot read '") +
                               path + "'");
        }
        text.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    // the S03 atomic write: a hidden temporary sibling, synced and renamed over the file (a
    // crash or a full disk mid-write leaves the old file whole; a read-only file is refused)
    bool writeTextFile(const std::string& path, const std::string& text, std::string* error){
        // atomic (S03); the folder must exist, as before (a typo'd folder is an error, not made)
        io::AtomicWriteOptions options;
        options.createDirectories = false;
        const io::WriteResult written = io::writeFileAtomic(utf8Path(FileData::getSystemPath(path)), text, options);
        if (!written.ok()){
            return fail(error, "cannot write '" + path + "': " + io::describeWriteError(written));
        }
        return true;
    }

    bool loadPoseAnimationFile(const std::string& path, PoseAnimationData& out,
                               std::vector<std::string>& warnings, std::string* error,
                               std::optional<PoseAnimationTemplateInfo>* templateInfo){
        std::string text;
        if (!readTextFile(path, text, error)){
            return false;
        }
        std::string parseError;
        if (!parsePoseAnimation(text, out, warnings, &parseError, templateInfo)){
            return fail(error, path + ": " + parseError);
        }
        return true;
    }

    bool savePoseAnimationFile(const std::string& path, const PoseAnimationData& data, std::string* error,
                               const PoseAnimationTemplateInfo* templateInfo){
        return writeTextFile(path, writePoseAnimation(data, templateInfo), error);
    }

    bool loadHumanoidOverridesFile(const std::string& path, HumanoidOverrides& out,
                                   std::vector<std::string>& warnings, std::string* error, bool* notFound){
        if (notFound){
            *notFound = false;
        }
        std::string text;
        bool missing = false;
        if (!readTextFile(path, text, error, &missing)){
            if (missing){
                out = HumanoidOverrides();
                if (notFound){
                    *notFound = true;
                }
                if (error){
                    error->clear();
                }
                return true;
            }
            return false;
        }
        std::string parseError;
        if (!parseHumanoidOverrides(text, out, warnings, &parseError)){
            return fail(error, path + ": " + parseError);
        }
        return true;
    }

    bool saveHumanoidOverridesFile(const std::string& path, const HumanoidOverrides& overrides, std::string* error){
        return writeTextFile(path, writeHumanoidOverrides(overrides), error);
    }

}
