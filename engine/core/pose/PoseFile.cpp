// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseFile.h"

#include "io/Data.h"
#include "io/File.h"
#include "io/FileData.h"

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

    bool writeTextFile(const std::string& path, const std::string& text, std::string* error){
        const std::string systemPath = FileData::getSystemPath(path);
        const std::string temporary = systemPath + ".tmp";
        {
            File file;
            if (file.open(temporary.c_str(), true) != FileErrors::FILEDATA_OK){
                return fail(error, "cannot write '" + path + "'");
            }
            const unsigned int size = static_cast<unsigned int>(text.size());
            const bool written = size == 0 ||
                                 file.write(reinterpret_cast<unsigned char*>(const_cast<char*>(text.data())), size) == size;
            file.flush();   // the File destructor closes the handle (close() would be closed twice)
            if (!written){
                std::error_code ignored;
                fs::remove(utf8Path(temporary), ignored);
                return fail(error, "failed writing '" + path + "'");
            }
        }
        std::error_code code;
        fs::rename(utf8Path(temporary), utf8Path(systemPath), code);
        if (code){
            fs::remove(utf8Path(temporary), code);
            return fail(error, "cannot replace '" + path + "'");
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
