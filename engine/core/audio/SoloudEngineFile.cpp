// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SoloudEngineFile.h"

#include "io/FileData.h"

using namespace jasperengine;

SoloudEngineFile::SoloudEngineFile(const std::string& path) {
    opened = file.open(path.c_str()) == FileErrors::FILEDATA_OK && file.getFilePtr() != nullptr;
}

int SoloudEngineFile::eof() {
    return opened ? file.eof() : 1;
}

unsigned int SoloudEngineFile::read(unsigned char* aDst, unsigned int aBytes) {
    return opened ? file.read(aDst, aBytes) : 0;
}

unsigned int SoloudEngineFile::length() {
    return opened ? file.length() : 0;
}

void SoloudEngineFile::seek(int aOffset) {
    if (opened) file.seek(aOffset);
}

unsigned int SoloudEngineFile::pos() {
    return opened ? file.pos() : 0;
}

FILE* SoloudEngineFile::getFilePtr() {
    return opened ? file.getFilePtr() : nullptr;
}

EngineWavStream::~EngineWavStream() {
    // the voices go first: they may still read their own files
    stop();
    mStreamFile = nullptr;
    delete parsed;
}

bool EngineWavStream::open(const std::string& filePath, SoLoud::result* result) {
    stop();
    mStreamFile = nullptr;
    delete parsed;
    parsed = nullptr;
    path = filePath;

    auto* file = new SoloudEngineFile(path);
    if (!file->isOpen()) {
        delete file;
        if (result) *result = SoLoud::FILE_NOT_FOUND;
        return false;
    }
    SoLoud::result res = loadFile(file);
    if (res != SoLoud::SO_NO_ERROR) {
        mStreamFile = nullptr;
        delete file;
        if (result) *result = res;
        return false;
    }
    parsed = file;
    if (result) *result = SoLoud::SO_NO_ERROR;
    return true;
}

SoLoud::AudioSourceInstance* EngineWavStream::createInstance() {
    // WavStreamInstance reads mStreamFile when it is made: this voice gets a file of its own,
    // which it deletes when it ends (its file is not mStreamFile any more by then)
    auto* own = new SoloudEngineFile(path);
    if (!own->isOpen()) {
        // the file went away since it was parsed: SoLoud's own way, the parsed file shared
        delete own;
        return new SoLoud::WavStreamInstance(this);
    }
    SoLoud::File* shared = mStreamFile;
    mStreamFile = own;
    auto* instance = new SoLoud::WavStreamInstance(this);
    mStreamFile = shared;
    return instance;
}
