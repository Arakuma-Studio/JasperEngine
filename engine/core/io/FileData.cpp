//
// Inspired by work of Jari Komppa in SoLoud audio engine
// https://sol.gfxile.net/soloud/file.html
// Modified by (c) 2020 Eduardo Doria.
//

#include "FileData.h"
#include "Data.h"
#include "ResourcePack.h"
#include "System.h"
#include <vector>

using namespace jasperengine;

FileData::~FileData(){

}

FileData* FileData::newFile(bool useHandle){
    if (!useHandle)
        return new Data();
    else
        return new File();
}

FileData* FileData::newFile(const char *aFilename, bool useHandle){
    // A packed resource has no file to hand a handle to, so it comes back as memory
    if (!useHandle || (aFilename && ResourcePack::contains(aFilename)))
        return new Data(aFilename);

    return new File(aFilename);
}

unsigned int FileData::read8(){
    unsigned char d = 0;
    read((unsigned char*)&d, 1);
    return d;
}

unsigned int FileData::read16(){
    unsigned short d = 0;
    read((unsigned char*)&d, 2);
    return d;
}

unsigned int FileData::read32(){
    unsigned int d = 0;
    read((unsigned char*)&d, 4);
    return d;
}

bool FileData::beginWith(const std::string& path, const std::string& prefix){
    return path.compare(0, prefix.size(), prefix) == 0;
}

std::string FileData::getBaseDir(const std::string& filepath){
    size_t found = filepath.find_last_of("/\\");

    if (found == std::string::npos)
        return "";

    std::string baseDir = filepath.substr(0, found);

    if (!baseDir.empty() && baseDir.back() != '/' && baseDir.back() != '\\') {
        baseDir += System::instance().getDirSeparator();
    }

    return baseDir;
}

std::string FileData::simplifyPath(const std::string& path) {

    std::vector<std::string> parts;

    std::string dir;

    std::string res;

    int len_path = path.length();

    for (int i = 0; i < len_path; i++) {

        dir.clear();

        while (path[i] == System::instance().getDirSeparator())
            i++;

        while (i < len_path && path[i] != System::instance().getDirSeparator()) {
            dir.push_back(path[i]);
            i++;
        }

        if (dir.compare("..") == 0) {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (dir.compare(".") == 0)
            continue;
        else if (dir.length() != 0)
            parts.push_back(dir);
    }

    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            res += System::instance().getDirSeparator();
        res += parts[i];
    }

    return res;
}

std::string FileData::getFilePathExtension(const std::string &filepath) {
    const size_t dot = filepath.find_last_of(".");
    if (dot != std::string::npos)
        return filepath.substr(dot + 1);
    return "";
}

std::string FileData::getSystemPath(std::string path){
    // Return full/absolute paths unchanged
    if (beginWith(path, "/") || (path.length() > 1 && path[1] == ':')){
        return path;
    }

    if (beginWith(path, "data://")){
        path = path.substr(7, path.length());
        return System::instance().getUserDataPath() + "/" + FileData::simplifyPath(path);
    }
    if (beginWith(path, "asset://")){
        path = path.substr(8, path.length());
        return System::instance().getAssetPath() + "/" + FileData::simplifyPath(path);
    }
    if (beginWith(path, "lua://")){
        path = path.substr(6, path.length());
        return System::instance().getLuaPath() + "/" + FileData::simplifyPath(path);
    }
    if (beginWith(path, "shader://")){
        path = path.substr(9, path.length());
        return System::instance().getShaderPath() + "/" + FileData::simplifyPath(path);
    }

    return System::instance().getAssetPath() + "/" + FileData::simplifyPath(path);
}

std::string FileData::readString(){
    return readString(length());
}

std::string FileData::readString(unsigned int stringlen){
    std::string s( stringlen, '\0' );
    read((unsigned char*)&s[0], stringlen);
    return s;
}

unsigned int FileData::writeString(const std::string& s){
    return write((unsigned char*)&s[0], (unsigned int)s.length());
}
