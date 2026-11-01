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

namespace {

    // a voice of a stream whose file went away: silent, and over at once
    class EndedInstance : public SoLoud::AudioSourceInstance {
    public:
        unsigned int getAudio(float* buffer, unsigned int, unsigned int bufferSize) override {
            for (unsigned int i = 0; i < bufferSize * mChannels; i++) buffer[i] = 0.0f;
            return 0;
        }
        bool hasEnded() override { return true; }
    };

}

SoLoud::AudioSourceInstance* EngineWavStream::createInstance() {
    // every voice reads a file of its own (openInstanceFile; the parsed file is never shared, so
    // no voice reads over another's position and none deletes another's file)
    auto* own = new SoloudEngineFile(path);
    if (!own->isOpen()) {
        delete own;
        return new EndedInstance();
    }
    pendingFile = own;
    auto* instance = new SoLoud::WavStreamInstance(this);
    // taken by the instance (or deleted by it when its decoder did not open)
    pendingFile = nullptr;
    return instance;
}

SoLoud::File* EngineWavStream::openInstanceFile() {
    SoLoud::File* file = pendingFile;
    pendingFile = nullptr;
    return file;
}
